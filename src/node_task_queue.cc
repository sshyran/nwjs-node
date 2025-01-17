#include "env-inl.h"
#include "node.h"
#include "node_errors.h"
#include "node_internals.h"
#include "node_process.h"
#include "util-inl.h"
#include "v8.h"

#include <atomic>

namespace node {

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::kPromiseHandlerAddedAfterReject;
using v8::kPromiseRejectAfterResolved;
using v8::kPromiseRejectWithNoHandler;
using v8::kPromiseResolveAfterResolved;
using v8::Local;
using v8::Message;
using v8::Number;
using v8::Object;
using v8::Promise;
using v8::PromiseRejectEvent;
using v8::PromiseRejectMessage;
using v8::Value;

namespace task_queue {

static void EnqueueMicrotask(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();

  CHECK(args[0]->IsFunction());

  isolate->EnqueueMicrotask(args[0].As<Function>());
}

// Should be in sync with runNextTicks in internal/process/task_queues.js
bool RunNextTicksNative(Environment* env) {
  TickInfo* tick_info = env->tick_info();
  if (!tick_info->has_tick_scheduled() && !tick_info->has_rejection_to_warn())
    v8::MicrotasksScope::PerformCheckpoint(env->isolate());
  if (!tick_info->has_tick_scheduled() && !tick_info->has_rejection_to_warn())
    return true;

  Local<Function> callback = env->tick_callback_function();
  CHECK(!callback.IsEmpty());
  return !callback->Call(env->context(), env->process_object(), 0, nullptr)
              .IsEmpty();
}

static void RunMicrotasks(const FunctionCallbackInfo<Value>& args) {
  //args.GetIsolate()->RunMicrotasks();
  v8::MicrotasksScope::PerformCheckpoint(args.GetIsolate());
}

static void SetTickCallback(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsFunction());
  env->set_tick_callback_function(args[0].As<Function>());
}

void PromiseRejectCallback(PromiseRejectMessage message) {
  static std::atomic<uint64_t> unhandledRejections{0};
  static std::atomic<uint64_t> rejectionsHandledAfter{0};

  Local<Promise> promise = message.GetPromise();
  Isolate* isolate = promise->GetIsolate();
  PromiseRejectEvent event = message.GetEvent();

  Environment* env = Environment::GetCurrent(isolate);

  if (env == nullptr) return;

  Local<Function> callback = env->promise_reject_callback();
  // The promise is rejected before JS land calls SetPromiseRejectCallback
  // to initializes the promise reject callback during bootstrap.
  CHECK(!callback.IsEmpty());

  Local<Value> value;
  Local<Value> type = Number::New(env->isolate(), event);

  if (event == kPromiseRejectWithNoHandler) {
    value = message.GetValue();
    unhandledRejections++;
    TRACE_COUNTER2(TRACING_CATEGORY_NODE2(promises, rejections),
                  "rejections",
                  "unhandled", unhandledRejections,
                  "handledAfter", rejectionsHandledAfter);
  } else if (event == kPromiseHandlerAddedAfterReject) {
    value = Undefined(isolate);
    rejectionsHandledAfter++;
    TRACE_COUNTER2(TRACING_CATEGORY_NODE2(promises, rejections),
                  "rejections",
                  "unhandled", unhandledRejections,
                  "handledAfter", rejectionsHandledAfter);
  } else if (event == kPromiseResolveAfterResolved) {
    value = message.GetValue();
  } else if (event == kPromiseRejectAfterResolved) {
    value = message.GetValue();
  } else {
    return;
  }

  if (value.IsEmpty()) {
    value = Undefined(isolate);
  }

  Local<Value> args[] = { type, promise, value };
  USE(callback->Call(
      env->context(), Undefined(isolate), arraysize(args), args));
}

static void SetPromiseRejectCallback(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsFunction());
  env->set_promise_reject_callback(args[0].As<Function>());
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  env->SetMethod(target, "enqueueMicrotask", EnqueueMicrotask);
  env->SetMethod(target, "setTickCallback", SetTickCallback);
  env->SetMethod(target, "runMicrotasks", RunMicrotasks);
  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(isolate, "tickInfo"),
              env->tick_info()->fields().GetJSArray()).Check();

  Local<Object> events = Object::New(isolate);
  NODE_DEFINE_CONSTANT(events, kPromiseRejectWithNoHandler);
  NODE_DEFINE_CONSTANT(events, kPromiseHandlerAddedAfterReject);
  NODE_DEFINE_CONSTANT(events, kPromiseResolveAfterResolved);
  NODE_DEFINE_CONSTANT(events, kPromiseRejectAfterResolved);

  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(isolate, "promiseRejectEvents"),
              events).Check();
  env->SetMethod(target,
                 "setPromiseRejectCallback",
                 SetPromiseRejectCallback);
}

}  // namespace task_queue
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(task_queue, node::task_queue::Initialize)
