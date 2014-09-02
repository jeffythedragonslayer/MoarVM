#include "moar.h"

/* Gets the current thread's profiling data structure, creating it if needed. */
static MVMProfileThreadData * get_thread_data(MVMThreadContext *tc) {
    if (!tc->prof_data) {
        tc->prof_data = calloc(1, sizeof(MVMProfileThreadData));
        tc->prof_data->start_time = uv_hrtime();
    }
    return tc->prof_data;
}

/* Log that we're entering a new frame. */
void MVM_profile_log_enter(MVMThreadContext *tc, MVMStaticFrame *sf, MVMuint64 mode) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Try to locate the entry node, if it's in the call graph already. */
    MVMProfileCallNode *pcn = NULL;
    MVMuint32 i;
    if (ptd->current_call)
        for (i = 0; i < ptd->current_call->num_succ; i++)
            if (ptd->current_call->succ[i]->sf == sf)
                pcn = ptd->current_call->succ[i];

    /* If we didn't find a call graph node, then create one and add it to the
     * graph. */
    if (!pcn) {
        pcn     = calloc(1, sizeof(MVMProfileCallNode));
        pcn->sf = sf;
        if (ptd->current_call) {
            MVMProfileCallNode *pred = ptd->current_call;
            pcn->pred = pred;
            if (pred->num_succ == pred->alloc_succ) {
                pred->alloc_succ += 8;
                pred->succ = realloc(pred->succ,
                    pred->alloc_succ * sizeof(MVMProfileCallNode *));
            }
            pred->succ[pred->num_succ] = pcn;
            pred->num_succ++;
        }
        else {
            if (!ptd->call_graph)
                ptd->call_graph = pcn;
        }
    }

    /* Increment entry counts. */
    pcn->total_entries++;
    switch (mode) {
        case MVM_PROFILE_ENTER_SPESH:
            pcn->specialized_entries++;
            break;
        case MVM_PROFILE_ENTER_SPESH_INLINE:
            pcn->specialized_entries++;
            pcn->inlined_entries++;
            break;
        case MVM_PROFILE_ENTER_JIT:
            pcn->jit_entries++;
            break;
        case MVM_PROFILE_ENTER_JIT_INLINE:
            pcn->jit_entries++;
            pcn->inlined_entries++;
            break;
    }
    pcn->entry_mode = mode;

    /* Log entry time; clear skip time. */
    pcn->cur_entry_time = uv_hrtime();
    pcn->cur_skip_time  = 0;

    /* The current call graph node becomes this one. */
    ptd->current_call = pcn;
}

/* Frame exit handler, used for unwind and normal exit. */
static void log_exit(MVMThreadContext *tc, MVMuint32 unwind) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Ensure we've a current frame; panic if not. */
    /* XXX in future, don't panic, try to cope. This is for debugging
     * profiler issues. */
    MVMProfileCallNode *pcn = ptd->current_call;
    if (!pcn /*|| !unwind && pcn->sf != tc->cur_frame->static_info*/) {
        MVM_dump_backtrace(tc);
        MVM_panic(1, "Profiler lost sequence");
    }

    /* Add to total time. */
    pcn->total_time += (uv_hrtime() - pcn->cur_entry_time) - pcn->cur_skip_time;

    /* Move back to predecessor in call graph. */
    ptd->current_call = pcn->pred;
}

/* Log that we're exiting a frame normally. */
void MVM_profile_log_exit(MVMThreadContext *tc) {
    log_exit(tc, 0);
}

/* Called when we unwind. Since we're also potentially leaving some inlined
 * frames, we need to exit until we hit the target one. */
void MVM_profile_log_unwind(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd  = get_thread_data(tc);
    MVMProfileCallNode   *lpcn;
    do {
        MVMProfileCallNode *pcn  = ptd->current_call;
        if (!pcn)
            return;
        lpcn = pcn;
        log_exit(tc, 1);
    } while (lpcn->sf != tc->cur_frame->static_info);
}

/* Called when we take a continuation. Leaves the static frames from the point
 * of view of the profiler, and saves each of them. */
MVMProfileContinuationData * MVM_profile_log_continuation_control(MVMThreadContext *tc, MVMFrame *root_frame) {
    MVMProfileThreadData        *ptd       = get_thread_data(tc);
    MVMProfileContinuationData  *cd        = MVM_malloc(sizeof(MVMProfileContinuationData));
    MVMStaticFrame             **sfs       = NULL;
    MVMuint64                   *modes     = NULL;
    MVMFrame                    *cur_frame = tc->cur_frame;
    MVMuint64                    alloc_sfs = 0;
    MVMuint64                    num_sfs   = 0;
    MVMFrame                   *last_frame;

    do {
        MVMProfileCallNode   *lpcn;
        do {
            MVMProfileCallNode *pcn = ptd->current_call;
            if (!pcn)
                MVM_panic(1, "Profiler lost sequence in continuation control");

            if (num_sfs == alloc_sfs) {
                alloc_sfs += 16;
                sfs        = realloc(sfs, alloc_sfs * sizeof(MVMStaticFrame *));
                modes      = realloc(modes, alloc_sfs * sizeof(MVMuint64));
            }
            sfs[num_sfs]   = pcn->sf;
            modes[num_sfs] = pcn->entry_mode;
            num_sfs++;

            lpcn = pcn;
            log_exit(tc, 1);
        } while (lpcn->sf != cur_frame->static_info);

        last_frame = cur_frame;
        cur_frame = cur_frame->caller;
    } while (last_frame != root_frame);

    cd->sfs     = sfs;
    cd->num_sfs = num_sfs;
    cd->modes   = modes;
    return cd;
}

/* Called when we invoke a continuation. Enters all the static frames we left
 * at the point we took the continuation. */
void MVM_profile_log_continuation_invoke(MVMThreadContext *tc, MVMProfileContinuationData *cd) {
    MVMuint64 i = cd->num_sfs;
    while (i--)
        MVM_profile_log_enter(tc, cd->sfs[i], cd->modes[i]);
}

/* Log that we've just allocated the passed object (just log the type). */
void MVM_profile_log_allocated(MVMThreadContext *tc, MVMObject *obj) {
    MVMProfileThreadData *ptd  = get_thread_data(tc);
    MVMProfileCallNode   *pcn  = ptd->current_call;
    MVMObject            *what = STABLE(obj)->WHAT;
    if (pcn) {
        /* See if there's an existing node to update. */
        MVMuint32 i;
        for (i = 0; i < pcn->num_alloc; i++) {
            if (pcn->alloc[i].type == what) {
                pcn->alloc[i].allocations++;
                return;
            }
        }

        /* No entry; create one. */
        if (pcn->num_alloc == pcn->alloc_alloc) {
            pcn->alloc_alloc += 8;
            pcn->alloc = realloc(pcn->alloc,
                pcn->alloc_alloc * sizeof(MVMProfileAllocationCount));
        }
        pcn->alloc[pcn->num_alloc].type        = what;
        pcn->alloc[pcn->num_alloc].allocations = 1;
        pcn->num_alloc++;
    }
}

/* Logs the start of a GC run. */
void MVM_profiler_log_gc_start(MVMThreadContext *tc, MVMuint32 full) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Make a new entry in the GCs. We use the cleared_bytes to store the
     * maximum that could be cleared, and after GC is done will subtract
     * retained bytes and promoted bytes. */
    if (ptd->num_gcs == ptd->alloc_gcs) {
        ptd->alloc_gcs += 16;
        ptd->gcs = realloc(ptd->gcs, ptd->alloc_gcs * sizeof(MVMProfileGC));
    }
    ptd->gcs[ptd->num_gcs].full          = full;
    ptd->gcs[ptd->num_gcs].cleared_bytes = (char *)tc->nursery_alloc -
                                           (char *)tc->nursery_tospace;

    /* Zero promoted bytes counter. */
    tc->gc_promoted_bytes = 0;

    /* Record start time. */
    ptd->cur_gc_start_time = uv_hrtime();
}

/* Logs the end of a GC run. */
void MVM_profiler_log_gc_end(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    MVMuint64 gc_time;
    MVMint32  retained_bytes;

    /* Record time spent. */
    gc_time = uv_hrtime() - ptd->cur_gc_start_time;
    ptd->gcs[ptd->num_gcs].time = gc_time;

    /* Record retained and promoted bytes. */
    retained_bytes = (char *)tc->nursery_alloc - (char *)tc->nursery_tospace;
    ptd->gcs[ptd->num_gcs].promoted_bytes = tc->gc_promoted_bytes;
    ptd->gcs[ptd->num_gcs].retained_bytes = retained_bytes;

    /* Tweak cleared bytes count. */
    ptd->gcs[ptd->num_gcs].cleared_bytes -= (retained_bytes + tc->gc_promoted_bytes);

    /* Increment the number of GCs we've done. */
    ptd->num_gcs++;

    /* Discount GC time from all active frames. */
    while (pcn) {
        pcn->cur_skip_time += gc_time;
        pcn = pcn->pred;
    }
}

/* Log that we're starting some work on bytecode specialization or JIT. */
void MVM_profiler_log_spesh_start(MVMThreadContext *tc) {
    /* Record start time. */
    MVMProfileThreadData *ptd = get_thread_data(tc);
    ptd->cur_spesh_start_time = uv_hrtime();
}

/* Log that we've finished doing bytecode specialization or JIT. */
void MVM_profiler_log_spesh_end(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    MVMuint64 spesh_time;

    /* Record time spent. */
    spesh_time = uv_hrtime() - ptd->cur_spesh_start_time;
    ptd->spesh_time += spesh_time;

    /* Discount spesh time from all active frames. */
    while (pcn) {
        pcn->cur_skip_time += spesh_time;
        pcn = pcn->pred;
    }
}

/* Log that an on stack replacement took place. */
void MVM_profiler_log_osr(MVMThreadContext *tc, MVMuint64 jitted) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn) {
        pcn->osr_count++;
        if (jitted)
            pcn->jit_entries++;
        else
            pcn->specialized_entries++;
    }
}

/* Log that local deoptimization took pace. */
void MVM_profiler_log_deopt_one(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn)
        pcn->deopt_one_count++;
}

/* Log that full-stack deoptimization took pace. */
void MVM_profiler_log_deopt_all(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn)
        pcn->deopt_all_count++;
}
