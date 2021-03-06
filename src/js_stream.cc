#include "js_stream.h"

#include "async_wrap.h"
#include "env-inl.h"
#include "node_errors.h"
#include "stream_base-inl.h"
#include "util-inl.h"
#include "v8.h"

namespace node {

using errors::TryCatchScope;

using v8::Array;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int32;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;


JSStream::JSStream(Environment* env, Local<Object> obj)
    : AsyncWrap(env, obj, AsyncWrap::PROVIDER_JSSTREAM),
      StreamBase(env) {
  MakeWeak();
  StreamBase::AttachToObject(obj);
}


AsyncWrap* JSStream::GetAsyncWrap() {
  return static_cast<AsyncWrap*>(this);
}


bool JSStream::IsAlive() {
  return true;
}


bool JSStream::IsClosing() {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  TryCatchScope try_catch(env());
  Local<Value> value;
  if (!MakeCallback(env()->isclosing_string(), 0, nullptr).ToLocal(&value)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated())
      errors::TriggerUncaughtException(env()->isolate(), try_catch);
    return true;
  }
  return value->IsTrue();
}


int JSStream::ReadStart() {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  TryCatchScope try_catch(env());
  Local<Value> value;
  int value_int = UV_EPROTO;
  if (!MakeCallback(env()->onreadstart_string(), 0, nullptr).ToLocal(&value) ||
      !value->Int32Value(env()->context()).To(&value_int)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated())
      errors::TriggerUncaughtException(env()->isolate(), try_catch);
  }
  return value_int;
}


int JSStream::ReadStop() {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  TryCatchScope try_catch(env());
  Local<Value> value;
  int value_int = UV_EPROTO;
  if (!MakeCallback(env()->onreadstop_string(), 0, nullptr).ToLocal(&value) ||
      !value->Int32Value(env()->context()).To(&value_int)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated())
      errors::TriggerUncaughtException(env()->isolate(), try_catch);
  }
  return value_int;
}


int JSStream::DoShutdown(ShutdownWrap* req_wrap) {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());

  Local<Value> argv[] = {
    req_wrap->object()
  };

  TryCatchScope try_catch(env());
  Local<Value> value;
  int value_int = UV_EPROTO;
  if (!MakeCallback(env()->onshutdown_string(),
                    arraysize(argv),
                    argv).ToLocal(&value) ||
      !value->Int32Value(env()->context()).To(&value_int)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated())
      errors::TriggerUncaughtException(env()->isolate(), try_catch);
  }
  return value_int;
}


int JSStream::DoWrite(WriteWrap* w,
                      uv_buf_t* bufs,
                      size_t count,
                      uv_stream_t* send_handle) {
  CHECK_NULL(send_handle);

  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());

  MaybeStackBuffer<Local<Value>, 16> bufs_arr(count);
  for (size_t i = 0; i < count; i++) {
    bufs_arr[i] =
        Buffer::Copy(env(), bufs[i].base, bufs[i].len).ToLocalChecked();
  }

  Local<Value> argv[] = {
    w->object(),
    Array::New(env()->isolate(), bufs_arr.out(), count)
  };

  TryCatchScope try_catch(env());
  Local<Value> value;
  int value_int = UV_EPROTO;
  if (!MakeCallback(env()->onwrite_string(),
                    arraysize(argv),
                    argv).ToLocal(&value) ||
      !value->Int32Value(env()->context()).To(&value_int)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated())
      errors::TriggerUncaughtException(env()->isolate(), try_catch);
  }
  return value_int;
}


void JSStream::New(const FunctionCallbackInfo<Value>& args) {
  // This constructor should not be exposed to public javascript.
  // Therefore we assert that we are not trying to call this as a
  // normal function.
  CHECK(args.IsConstructCall());
  Environment* env = Environment::GetCurrent(args);
  new JSStream(env, args.This());
}


template <class Wrap>
void JSStream::Finish(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsObject());
  Wrap* w = static_cast<Wrap*>(StreamReq::FromObject(args[0].As<Object>()));

  CHECK(args[1]->IsInt32());
  w->Done(args[1].As<Int32>()->Value());
}


void JSStream::ReadBuffer(const FunctionCallbackInfo<Value>& args) {
  JSStream* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  ArrayBufferViewContents<char> buffer(args[0]);
  const char* data = buffer.data();
  int len = buffer.length();

  // Repeatedly ask the stream's owner for memory, copy the data that we
  // just read from JS into those buffers and emit them as reads.
  while (len != 0) {
    uv_buf_t buf = wrap->EmitAlloc(len);
    ssize_t avail = len;
    if (static_cast<ssize_t>(buf.len) < avail)
      avail = buf.len;

    memcpy(buf.base, data, avail);
    data += avail;
    len -= avail;
    wrap->EmitRead(avail, buf);
  }
}


void JSStream::EmitEOF(const FunctionCallbackInfo<Value>& args) {
  JSStream* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  wrap->EmitRead(UV_EOF);
}


void JSStream::Initialize(Local<Object> target,
                          Local<Value> unused,
                          Local<Context> context,
                          void* priv) {
  Environment* env = Environment::GetCurrent(context);

  Local<FunctionTemplate> t = env->NewFunctionTemplate(New);
  Local<String> jsStreamString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "JSStream");
  t->SetClassName(jsStreamString);
  t->InstanceTemplate()
    ->SetInternalFieldCount(StreamBase::kStreamBaseFieldCount);
  t->Inherit(AsyncWrap::GetConstructorTemplate(env));

  env->SetProtoMethod(t, "finishWrite", Finish<WriteWrap>);
  env->SetProtoMethod(t, "finishShutdown", Finish<ShutdownWrap>);
  env->SetProtoMethod(t, "readBuffer", ReadBuffer);
  env->SetProtoMethod(t, "emitEOF", EmitEOF);

  StreamBase::AddMethods(env, t);
  target->Set(env->context(),
              jsStreamString,
              t->GetFunction(context).ToLocalChecked()).Check();
}

}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(js_stream, node::JSStream::Initialize)
