#include <ruby.h>
#include <ruby/debug.h>
#include <signal.h>
#include <sys/time.h>

#define MAX_STACK_DEPTH 400 // FIXME: Need to handle when this is not enough

static VALUE native_working_p(VALUE self);
static VALUE sample_threads(VALUE self);
static VALUE do_sample_threads();
static VALUE sample_thread(VALUE thread);
static VALUE to_sample(int frames_count, VALUE* frames, int* lines);
static VALUE start_timer(VALUE self);
static void install_signal_handler(void);
static void handle_signal(int _signal, siginfo_t *_info, void *_ucontext);
static void job_callback(void* arg);

VALUE sampling_results;

// From borrowed_from_ruby.c
int borrowed_from_ruby_sources_rb_profile_frames(VALUE thread, int start, int limit, VALUE *buff, int *lines);
VALUE thread_id_for(VALUE thread);

// From Ruby internal.h
int ruby_thread_has_gvl_p(void);

void Init_ddtrace_profiling_native_extension(void) {
  VALUE datadog_module = rb_define_module("Datadog");
  VALUE profiling_module = rb_define_module_under(datadog_module, "Profiling");
  VALUE native_extension_module = rb_define_module_under(profiling_module, "NativeExtension");

  rb_define_singleton_method(native_extension_module, "native_working?", native_working_p, 0);
  rb_funcall(native_extension_module, rb_intern("private_class_method"), 1, ID2SYM(rb_intern("native_working?")));

  rb_define_singleton_method(native_extension_module, "sample_threads", sample_threads, 0);
  rb_define_singleton_method(native_extension_module, "start_timer", start_timer, 0);

  sampling_results = rb_ary_new();
  rb_define_variable("$sampling_results", &sampling_results);
}

static VALUE native_working_p(VALUE self) {
  return Qtrue;
}

static VALUE sample_threads(VALUE self) {
  return do_sample_threads();
}

static VALUE do_sample_threads() {
  if (!ruby_thread_has_gvl_p()) {
    rb_fatal("Expected to have GVL");
  }

  VALUE threads = rb_funcall(rb_cThread, rb_intern("list"), 0);
  VALUE samples = rb_ary_new();

  for (int i = 0; i < RARRAY_LEN(threads); i++) {
    VALUE thread = RARRAY_AREF(threads, i);
    VALUE result = sample_thread(thread);

    rb_ary_push(samples, result);
  }

  return samples;
}

static VALUE sample_thread(VALUE thread) {
  VALUE frames[MAX_STACK_DEPTH];
  int lines[MAX_STACK_DEPTH];

  int stack_depth = borrowed_from_ruby_sources_rb_profile_frames(thread, 0, MAX_STACK_DEPTH, frames, lines);
  VALUE stack = to_sample(stack_depth, frames, lines);
  VALUE thread_id = thread_id_for(thread);

  return rb_ary_new_from_args(3, thread, thread_id, stack);
}

static VALUE to_sample(int frames_count, VALUE* frames, int* lines) {
  VALUE result = rb_ary_new();

  for (int i = 0; i < frames_count; i++) {
    rb_ary_push(result,
      rb_ary_new_from_args(3, rb_profile_frame_path(frames[i]), rb_profile_frame_full_label(frames[i]), INT2FIX(lines[i]))
    );
  }

  return result;
}

static VALUE start_timer(VALUE self) {
  install_signal_handler();

  struct itimerval timer_config;

  timer_config.it_interval.tv_sec = 1;
  timer_config.it_interval.tv_usec = 0;
  timer_config.it_value.tv_sec = 1;
  timer_config.it_value.tv_usec = 0;

  setitimer(ITIMER_REAL, &timer_config, NULL);

  return Qtrue;
}

static void install_signal_handler(void) {
  struct sigaction signal_handler_config;

  sigemptyset(&signal_handler_config.sa_mask);
  signal_handler_config.sa_handler = NULL;
  signal_handler_config.sa_flags = SA_RESTART | SA_SIGINFO;
  signal_handler_config.sa_sigaction = handle_signal;

  if (sigaction(SIGALRM, &signal_handler_config, NULL) != 0) {
    rb_fatal("Could not install signal handler");
  }
}

static void handle_signal(int _signal, siginfo_t *_info, void *_ucontext) {
  // fprintf(stderr, "Tick!\n");
  rb_postponed_job_register_one(0, job_callback, NULL);
}

static void job_callback(void* _payload) {
  // fprintf(stderr, "Job callback!\n");

  rb_ary_push(sampling_results, do_sample_threads());
}
