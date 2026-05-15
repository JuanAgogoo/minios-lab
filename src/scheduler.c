/*
 * scheduler.c — ESQUELETO DEL LABORATORIO
 *
 * Este archivo contiene el núcleo del scheduler round-robin de miniOS.
 * Las funciones de infraestructura (init, getters, install_sigchld, stop,
 * timespec_diff_ms) ya están implementadas.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. scheduler_create_process  — fork + exec + SIGSTOP + PCB init
 *   2. scheduler_start           — arrancar el primer proceso y el timer
 *   3. scheduler_tick            — handler de SIGALRM (context switch)
 *   4. scheduler_sigchld         — handler de SIGCHLD (terminación)
 *
 * Cada función viene con comentarios numerados que describen el flujo
 * paso a paso. Tu trabajo es traducir cada paso a código C usando las
 * APIs de POSIX y las funciones de infraestructura disponibles.
 *
 * APIs disponibles:
 *   - POSIX:       fork, execl, waitpid, kill, clock_gettime
 *   - platform_*:  ver src/platform/platform.h
 *   - pcb_*:       ver src/pcb.h
 *   - rq_*:        ver src/ready_queue.h
 *   - timer_*:     ver src/timer.h
 *   - monitor_*:   ver src/monitor.h
 *
 * REGLAS DE SEGURIDAD EN SEÑALES (importantes para scheduler_tick y
 * scheduler_sigchld):
 *   - NO uses printf/fprintf dentro de los handlers (no son
 *     async-signal-safe). Solo kill, waitpid, clock_gettime, write.
 *   - El shell bloquea SIGALRM con sigprocmask durante sus operaciones
 *     críticas, por lo que no necesitas mutex manual sobre process_table.
 */
 
#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>
 
// Estado global del scheduler
static volatile int current_running = -1;   // índice en process_table del proceso RUNNING, -1 si ninguno
static volatile int scheduler_active = 0;   // 1 si el scheduler está corriendo
 
// ============================================================
// Helpers ya implementados — NO los modifiques
// ============================================================
 
double timespec_diff_ms(struct timespec end, struct timespec start) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}
 
void scheduler_init(void) {
    process_count = 0;
    current_running = -1;
    scheduler_active = 0;
    rq_init();
}
 
int scheduler_get_running(void) {
    return current_running;
}
 
int scheduler_is_running(void) {
    return scheduler_active;
}
 
void scheduler_install_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}
 
void scheduler_stop(void) {
    timer_stop();
    scheduler_active = 0;
 
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_TERMINATED) {
            kill(process_table[i].pid, SIGKILL);
            int status;
            waitpid(process_table[i].pid, &status, 0);
            process_table[i].state = PROC_TERMINATED;
        }
    }
    current_running = -1;
}
 
 
// ============================================================
// [TODO 1/4] scheduler_create_process
// ------------------------------------------------------------
// Crea un proceso nuevo a partir de un binario, lo deja detenido
// con estado PROC_READY y lo encola en la ready queue.
//
// Retorna el índice del nuevo PCB en process_table, o -1 en error.
//
// Observable correcto: `ps aux | grep <binario>` debe mostrar el
// proceso en estado T (stopped) justo después de crearlo.
// ============================================================
int scheduler_create_process(const char *path, const char *arg) {
 
    /* Paso 1 — Validar espacio en la tabla */
    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "Error: proceso_table llena (max %d)\n", MAX_PROCESSES);
        return -1;
    }
 
    /* Paso 2 — fork() */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
 
    /* Paso 3 — Hijo: habilitar ptrace si aplica y ejecutar el binario */
    if (pid == 0) {
        if (platform_uses_ptrace()) {
            platform_trace_child();          /* En Linux: PTRACE_TRACEME */
        }
        if (arg != NULL) {
            execl(path, path, arg, NULL);
        } else {
            execl(path, path, NULL);
        }
        /* Si llegamos aquí, execl falló */
        perror("execl");
        _exit(1);
    }
 
    /* A partir de aquí: estamos en el PADRE (pid > 0) */
 
    int status;
 
    /* Paso 4 — Si usamos ptrace, esperar el SIGTRAP post-exec */
    if (platform_uses_ptrace()) {
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid (ptrace post-exec)");
            kill(pid, SIGKILL);
            return -1;
        }
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "Error: hijo no se detuvo tras exec\n");
            kill(pid, SIGKILL);
            return -1;
        }
    }
 
    /* Paso 5 — Inicializar el PCB */
    int idx = process_count;
    char *path_copy  = strdup(path);
    char *short_name = basename(path_copy);
    pcb_init(&process_table[idx], pid, short_name);
    free(path_copy);
 
    /* Paso 6 — Capturar registros iniciales (solo si ptrace disponible) */
    if (platform_uses_ptrace()) {
        if (platform_get_registers(pid, &process_table[idx].registers) == 0) {
            process_table[idx].regs_valid = 1;
        }
        platform_detach(pid);   /* Liberar el tracing para control manual */
    }
 
    /* Paso 7 — Detener el proceso con SIGSTOP */
    if (platform_stop_process(pid) != 0) {
        perror("platform_stop_process");
        kill(pid, SIGKILL);
        return -1;
    }
 
    /* Paso 8 — Confirmar que se detuvo */
    if (waitpid(pid, &status, WUNTRACED) < 0) {
        perror("waitpid (WUNTRACED confirm stop)");
        kill(pid, SIGKILL);
        return -1;
    }
 
    /* Paso 9 — Marcar READY, incrementar contador, encolar y emitir eventos */
    process_table[idx].state = PROC_READY;
    process_count++;
    rq_enqueue(idx);
    monitor_emit_created(pid, process_table[idx].name);
    if (process_table[idx].regs_valid) {
        monitor_emit_registers(
            pid,
            process_table[idx].registers.program_counter,
            process_table[idx].registers.stack_pointer
        );
    }
 
    printf("Proceso '%s' creado con PID %d (idx=%d)\n",
           process_table[idx].name, pid, idx);
    return idx;
}
 
 
/* ============================================================
 * [TODO 2/4] scheduler_start
 * ============================================================ */
void scheduler_start(int slice_ms) {
 
    /* Paso 1 — Verificar que haya procesos */
    if (rq_is_empty()) {
        printf("No hay procesos en la ready queue.\n");
        return;
    }
 
    /* Paso 2 — Desencolar el primer proceso */
    int idx = rq_dequeue();
 
    /* Paso 3 — Actualizar su PCB a RUNNING */
    process_table[idx].state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &process_table[idx].last_started);
    current_running = idx;
 
    /* Paso 4 — Reanudar el proceso */
    platform_resume_process(process_table[idx].pid);
    printf("Scheduler iniciado: PID %d corriendo con slice=%d ms\n",
           process_table[idx].pid, slice_ms);
 
    /* Paso 5 — Activar el scheduler y el timer */
    scheduler_active = 1;
    timer_init(slice_ms, scheduler_tick);   /* Registrar handler SIGALRM */
    timer_start();                          /* Armar setitimer */
}
 
 
/* ============================================================
 * [TODO 3/4] scheduler_tick  (handler de SIGALRM)
 * IMPORTANTE: solo funciones async-signal-safe dentro de aquí.
 * ============================================================ */
void scheduler_tick(int signum) {
    (void)signum;
 
    /* Paso 1 — Salida temprana si no hay nada corriendo */
    if (current_running < 0 || !scheduler_active) return;
 
    /* Paso 2 — Puntero al PCB del proceso actual */
    pcb_t *current = &process_table[current_running];
 
    /* Paso 3 — Detener el proceso actual */
    platform_stop_process(current->pid);
 
    /* Paso 4 — Actualizar métricas del proceso saliente */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = timespec_diff_ms(now, current->last_started);
    current->cpu_time_ms     += elapsed;
    current->state            = PROC_READY;
    current->context_switches++;
 
    /* Paso 5 — Devolver el proceso saliente a la cola */
    rq_enqueue(current_running);
 
    /* Paso 6 — Si la cola quedó vacía (solo había 1 proceso y terminó
     *           mientras esperábamos la señal), detener el timer */
    if (rq_is_empty()) {
        current_running = -1;
        timer_stop();
        return;
    }
 
    /* Paso 7 — Sacar al siguiente y reanudarlo */
    int next_idx   = rq_dequeue();
    pcb_t *next    = &process_table[next_idx];
    next->state    = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &next->last_started);
    platform_resume_process(next->pid);
 
    /* Emitir evento de cambio de contexto (usa write internamente, es safe) */
    monitor_emit_switch(current->pid, next->pid, timer_get_slice());
 
    current_running = next_idx;
}
 
 
/* ============================================================
 * [TODO 4/4] scheduler_sigchld  (handler de SIGCHLD)
 * IMPORTANTE: solo funciones async-signal-safe dentro de aquí.
 * ============================================================ */
void scheduler_sigchld(int signum) {
    (void)signum;
 
    int status;
    pid_t pid;
 
    /* Paso 1 — Recoger TODOS los hijos terminados sin bloquear */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
 
        /* Paso 2 — Ignorar paradas (SIGSTOP/SIGTSTP): solo nos interesan
         *           procesos que realmente terminaron */
        if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
            continue;
        }
 
        /* Paso 3 — Buscar el PID en process_table */
        int found_idx = -1;
        for (int i = 0; i < process_count; i++) {
            if (process_table[i].pid == pid &&
                process_table[i].state != PROC_TERMINATED) {
                found_idx = i;
                break;
            }
        }
        if (found_idx < 0) continue;   /* Ya marcado o desconocido */
 
        int i = found_idx;
 
        /* Paso 4 — Marcar como terminado y emitir evento */
        process_table[i].state = PROC_TERMINATED;
        monitor_emit_terminated(
            pid,
            process_table[i].cpu_time_ms,
            process_table[i].context_switches
        );
 
        /* Paso 5 — ¿Era el proceso en ejecución? */
        if (i == current_running) {
 
            /* 5a — Acumular el CPU time del último burst */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = timespec_diff_ms(now, process_table[i].last_started);
            process_table[i].cpu_time_ms += elapsed;
 
            /* 5b — Limpiar current_running */
            current_running = -1;
 
            /* 5c — Despachar siguiente si hay uno en cola */
            if (!rq_is_empty()) {
                int next = rq_dequeue();
                process_table[next].state = PROC_RUNNING;
                clock_gettime(CLOCK_MONOTONIC, &process_table[next].last_started);
                platform_resume_process(process_table[next].pid);
                current_running = next;
            } else {
                /* 5d — Cola vacía: apagar scheduler */
                timer_stop();
                scheduler_active = 0;
            }
 
        } else {
            /* Paso 6 — Estaba en la ready queue: removerlo */
            rq_remove(i);
        }
    }
    /* waitpid retornó 0 (nada más pendiente) o -1 (error/no hijos): fin */
}
