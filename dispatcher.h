// ------------------------------------------------------------------
//   dispatcher.h
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef DISPATCHER_INCLUDED
#define DISPATCHER_INCLUDED

#include "genozip.h"
#include "buffer.h"

typedef void *Dispatcher;
extern Dispatcher dispatcher_init (unsigned max_threads, unsigned previous_vb_i,
                                   bool test_mode, bool is_last_file, const char *filename);
extern void dispatcher_pause (Dispatcher dispatcher);
extern void dispatcher_resume (Dispatcher dispatcher);
extern void dispatcher_finish (Dispatcher *dispatcher, unsigned *last_vb_i);

extern void dispatcher_compute (Dispatcher dispatcher, void (*func)(VBlockP));
extern VBlockP dispatcher_generate_next_vb (Dispatcher dispatcher, uint32_t vb_i);       
extern bool dispatcher_has_processed_vb (Dispatcher dispatcher, bool *is_final);                                  
extern VBlockP dispatcher_get_processed_vb (Dispatcher dispatcher, bool *is_final);
extern bool dispatcher_has_free_thread (Dispatcher dispatcher);
extern VBlockP dispatcher_get_next_vb (Dispatcher dispatcher);
extern void dispatcher_finalize_one_vb (Dispatcher dispatcher);
extern void dispatcher_abandon_next_vb (Dispatcher dispatcher);
extern void dispatcher_input_exhausted (Dispatcher dispatcher);
extern bool dispatcher_is_done (Dispatcher dispatcher);
extern bool dispatcher_is_input_exhausted (Dispatcher dispatcher);
extern void dispatcher_show_time (const char *stage, int32_t thread_index, uint32_t vb_i);
extern const char *dispatcher_ellapsed_time (Dispatcher dispatcher, bool ever);

#endif
