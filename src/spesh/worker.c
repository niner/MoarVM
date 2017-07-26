#include "moar.h"

/* The specialization worker thread receives logs from other threads about
 * calls and types that showed up at runtime. It uses this to produce
 * specialized versions of code. */

/* Enters the work loop. */
static void worker(MVMThreadContext *tc, MVMCallsite *callsite, MVMRegister *args) {
    MVMObject *updated_static_frames = MVM_repr_alloc_init(tc,
        tc->instance->boot_types.BOOTArray);
    MVMObject *previous_static_frames = MVM_repr_alloc_init(tc,
        tc->instance->boot_types.BOOTArray);
    MVMROOT(tc, updated_static_frames, {
    MVMROOT(tc, previous_static_frames, {
        while (1) {
            MVMObject *log_obj;
            MVMuint64 start_time;
            unsigned int interval_id;
            if (tc->instance->spesh_log_fh)
                start_time = uv_hrtime();
            log_obj = MVM_repr_shift_o(tc, tc->instance->spesh_queue);
            if (tc->instance->spesh_log_fh) {
                fprintf(tc->instance->spesh_log_fh,
                    "Received Logs\n"
                    "=============\n\n"
                    "Was waiting %dus for logs on the log queue.\n\n",
                    (int)((uv_hrtime() - start_time) / 1000));
            }

            interval_id = MVM_telemetry_interval_start(tc, "spesh worker consuming a log");

            uv_mutex_lock(&(tc->instance->mutex_spesh_sync));
            tc->instance->spesh_working = 1;
            uv_mutex_unlock(&(tc->instance->mutex_spesh_sync));

            tc->instance->spesh_stats_version++;
            if (log_obj->st->REPR->ID == MVM_REPR_ID_MVMSpeshLog) {
                MVMSpeshLog *sl = (MVMSpeshLog *)log_obj;
                MVM_telemetry_interval_annotate((uintptr_t)sl->body.thread->body.tc, interval_id, "from this thread");
                MVMROOT(tc, sl, {
                    MVMThreadContext *stc;
                    MVMuint32 i;
                    MVMuint32 n;

                    /* Update stats, and if we're logging dump each of them. */
                    tc->instance->spesh_stats_version++;
                    if (tc->instance->spesh_log_fh)
                        start_time = uv_hrtime();
                    MVM_spesh_stats_update(tc, sl, updated_static_frames);
                    n = MVM_repr_elems(tc, updated_static_frames);
                    if (tc->instance->spesh_log_fh) {
                        fprintf(tc->instance->spesh_log_fh,
                            "Statistics Updated\n"
                            "==================\n"
                            "%d frames had their statistics updated in %dus.\n\n",
                            (int)n, (int)((uv_hrtime() - start_time) / 1000));
                        for (i = 0; i < n; i++) {
                            char *dump = MVM_spesh_dump_stats(tc, (MVMStaticFrame* )
                                MVM_repr_at_pos_o(tc, updated_static_frames, i));
                            fprintf(tc->instance->spesh_log_fh, "%s==========\n\n", dump);
                            MVM_free(dump);
                        }
                    }
                    MVM_telemetry_interval_annotate((uintptr_t)n, interval_id, "stats for this many frames");
                    GC_SYNC_POINT(tc);

                    /* Form a specialization plan. */
                    if (tc->instance->spesh_log_fh)
                        start_time = uv_hrtime();
                    tc->instance->spesh_plan = MVM_spesh_plan(tc, updated_static_frames);
                    if (tc->instance->spesh_log_fh) {
                        n = tc->instance->spesh_plan->num_planned;
                        fprintf(tc->instance->spesh_log_fh,
                            "Specialization Plan\n"
                            "===================\n"
                            "%u specialization(s) will be produced (planned in %dus).\n\n",
                            n, (int)((uv_hrtime() - start_time) / 1000));
                        for (i = 0; i < n; i++) {
                            char *dump = MVM_spesh_dump_planned(tc,
                                &(tc->instance->spesh_plan->planned[i]));
                            fprintf(tc->instance->spesh_log_fh, "%s==========\n\n", dump);
                            MVM_free(dump);
                        }
                    }
                    MVM_telemetry_interval_annotate((uintptr_t)tc->instance->spesh_plan->num_planned, interval_id,
                            "this many specializations planned");
                    GC_SYNC_POINT(tc);

                    /* Implement the plan and then discard it. */
                    n = tc->instance->spesh_plan->num_planned;
                    for (i = 0; i < n; i++) {
                        MVM_spesh_candidate_add(tc, &(tc->instance->spesh_plan->planned[i]));
                        GC_SYNC_POINT(tc);
                    }
                    MVM_spesh_plan_destroy(tc, tc->instance->spesh_plan);
                    tc->instance->spesh_plan = NULL;

                    /* Clear up stats that didn't get updated for a while,
                     * then add frames updated this time into the previously
                     * updated array. */
                    MVM_spesh_stats_cleanup(tc, previous_static_frames);
                    n = MVM_repr_elems(tc, updated_static_frames);
                    for (i = 0; i < n; i++)
                        MVM_repr_push_o(tc, previous_static_frames,
                            MVM_repr_at_pos_o(tc, updated_static_frames, i));

                    /* Clear updated static frames array. */
                    MVM_repr_pos_set_elems(tc, updated_static_frames, 0);

                    /* Allow the sending thread to produce more logs again,
                     * putting a new spesh log in place if needed. */
                    stc = sl->body.thread->body.tc;
                    if (stc)
                        if (MVM_incr(&(stc->spesh_log_quota)) == 0)
                            stc->spesh_log = MVM_spesh_log_create(tc, sl->body.thread);

                    /* If needed, signal sending thread that it can continue. */
                    if (sl->body.block_mutex) {
                        uv_mutex_lock(sl->body.block_mutex);
                        MVM_store(&(sl->body.completed), 1);
                        uv_cond_signal(sl->body.block_condvar);
                        uv_mutex_unlock(sl->body.block_mutex);
                    }
                });
            }
            else {
                MVM_panic(1, "Unexpected object sent to specialization worker");
            }

            MVM_telemetry_interval_stop(tc, interval_id, "spesh worker finished");

            uv_mutex_lock(&(tc->instance->mutex_spesh_sync));
            tc->instance->spesh_working = 0;
            uv_cond_broadcast(&(tc->instance->cond_spesh_sync));
            uv_mutex_unlock(&(tc->instance->mutex_spesh_sync));
        }
    });
    });
}

void MVM_spesh_worker_setup(MVMThreadContext *tc) {
    if (tc->instance->spesh_enabled) {
        MVMObject *worker_entry_point;
        tc->instance->spesh_queue = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTQueue);
        worker_entry_point = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTCCode);
        ((MVMCFunction *)worker_entry_point)->body.func = worker;
        MVM_thread_run(tc, MVM_thread_new(tc, worker_entry_point, 1));
    }
}
