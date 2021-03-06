/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <vector>

#include "grpc/support/log.h"

#include <nan.h>
#include <node.h>
#include "call.h"
#include "channel.h"
#include "channel_credentials.h"
#include "completion_queue.h"
#include "grpc/grpc.h"
#include "grpc/grpc_security.h"
#include "slice.h"
#include "timeval.h"

namespace grpc {
namespace node {

using Nan::Callback;
using Nan::EscapableHandleScope;
using Nan::HandleScope;
using Nan::Maybe;
using Nan::MaybeLocal;
using Nan::ObjectWrap;
using Nan::Persistent;
using Nan::Utf8String;

using v8::Array;
using v8::Exception;
using v8::Function;
using v8::FunctionTemplate;
using v8::Integer;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Value;

Callback *Channel::constructor;
Persistent<FunctionTemplate> Channel::fun_tpl;

static const char grpc_node_user_agent[] = "grpc-node/" GRPC_NODE_VERSION;

void PopulateUserAgentChannelArg(grpc_arg *arg) {
    size_t key_len = sizeof(GRPC_ARG_PRIMARY_USER_AGENT_STRING);
    size_t val_len = sizeof(grpc_node_user_agent);
    arg->key = reinterpret_cast<char *>(calloc(key_len, sizeof(char)));
    memcpy(arg->key, GRPC_ARG_PRIMARY_USER_AGENT_STRING, key_len);
    arg->type = GRPC_ARG_STRING;
    arg->value.string = reinterpret_cast<char *>(calloc(val_len, sizeof(char)));
    memcpy(arg->value.string, grpc_node_user_agent, val_len);

}

bool ParseChannelArgs(Local<Value> args_val,
                      grpc_channel_args **channel_args_ptr) {
  if (args_val->IsUndefined() || args_val->IsNull()) {
    // Treat null and undefined the same as an empty object
    args_val = Nan::New<Object>();
  }
  if (!args_val->IsObject()) {
    *channel_args_ptr = NULL;
    return false;
  }
  grpc_channel_args *channel_args =
      reinterpret_cast<grpc_channel_args *>(malloc(sizeof(grpc_channel_args)));
  *channel_args_ptr = channel_args;
  Local<Object> args_hash = Nan::To<Object>(args_val).ToLocalChecked();
  Local<Array> keys = Nan::GetOwnPropertyNames(args_hash).ToLocalChecked();
  channel_args->num_args = keys->Length();
  /* This is an ugly hack to add in the user agent string argument if it wasn't
   * passed by the user */
  bool has_user_agent_arg = Nan::HasOwnProperty(
        args_hash, Nan::New(GRPC_ARG_PRIMARY_USER_AGENT_STRING).ToLocalChecked()
  ).FromJust();
  if (!has_user_agent_arg) {
    channel_args->num_args += 1;
  }
  channel_args->args = reinterpret_cast<grpc_arg *>(
      calloc(channel_args->num_args, sizeof(grpc_arg)));
  for (unsigned int i = 0; i < keys->Length(); i++) {
    Local<Value> key = Nan::Get(keys, i).ToLocalChecked();
    Utf8String key_str(key);
    if (*key_str == NULL) {
      // Key string conversion failed
      return false;
    }
    Local<Value> value = Nan::Get(args_hash, key).ToLocalChecked();
    if (value->IsInt32()) {
      channel_args->args[i].type = GRPC_ARG_INTEGER;
      channel_args->args[i].value.integer = Nan::To<int32_t>(value).FromJust();
    } else if (value->IsString()) {
      Utf8String val_str(value);
      channel_args->args[i].type = GRPC_ARG_STRING;
      /* Append the grpc-node user agent string after the application user agent
       * string, and put the combination at the beginning of the user agent string
       */
      if (strcmp(*key_str, GRPC_ARG_PRIMARY_USER_AGENT_STRING) == 0) {
        /* val_str.length() is the string length and does not include the
         * trailing 0 byte. sizeof(grpc_node_user_agent) is the array length,
         * so it does include the trailing 0 byte. */
        size_t val_str_len = val_str.length();
        size_t user_agent_len = sizeof(grpc_node_user_agent);
        /* This is the length of the two parts of the string, plus the space in
         * between, plus the 0 at the end, which is included in user_agent_len.
         */
        channel_args->args[i].value.string =
            reinterpret_cast<char *>(calloc(val_str_len + user_agent_len + 1, sizeof(char)));
        memcpy(channel_args->args[i].value.string, *val_str,
               val_str.length());
        channel_args->args[i].value.string[val_str_len] = ' ';
        memcpy(channel_args->args[i].value.string + val_str_len + 1,
               grpc_node_user_agent, user_agent_len);
      } else {
        channel_args->args[i].value.string =
            reinterpret_cast<char *>(calloc(val_str.length() + 1, sizeof(char)));
        memcpy(channel_args->args[i].value.string, *val_str,
              val_str.length() + 1);
      }
    } else {
      // The value does not match either of the accepted types
      return false;
    }
    channel_args->args[i].key =
        reinterpret_cast<char *>(calloc(key_str.length() + 1, sizeof(char)));
    memcpy(channel_args->args[i].key, *key_str, key_str.length() + 1);
  }
  /* Add a standard user agent string argument if none was provided */
  if (!has_user_agent_arg) {
    size_t index = channel_args->num_args - 1;
    PopulateUserAgentChannelArg(&channel_args->args[index]);
  }
  return true;
}

void DeallocateChannelArgs(grpc_channel_args *channel_args) {
  if (channel_args == NULL) {
    return;
  }
  for (size_t i = 0; i < channel_args->num_args; i++) {
    if (channel_args->args[i].key == NULL) {
      /* NULL key implies that this argument and all subsequent arguments failed
       * to parse */
      break;
    }
    free(channel_args->args[i].key);
    if (channel_args->args[i].type == GRPC_ARG_STRING) {
      free(channel_args->args[i].value.string);
    }
  }
  free(channel_args->args);
  free(channel_args);
}

Channel::Channel(grpc_channel *channel) : wrapped_channel(channel) {}

Channel::~Channel() {
  gpr_log(GPR_DEBUG, "Destroying channel");
  if (wrapped_channel != NULL) {
    grpc_channel_destroy(wrapped_channel);
  }
}

void Channel::Init(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("Channel").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  Nan::SetPrototypeMethod(tpl, "close", Close);
  Nan::SetPrototypeMethod(tpl, "getTarget", GetTarget);
  Nan::SetPrototypeMethod(tpl, "getConnectivityState", GetConnectivityState);
  Nan::SetPrototypeMethod(tpl, "watchConnectivityState",
                          WatchConnectivityState);
  Nan::SetPrototypeMethod(tpl, "createCall", CreateCall);
  fun_tpl.Reset(tpl);
  Local<Function> ctr = Nan::GetFunction(tpl).ToLocalChecked();
  Nan::Set(exports, Nan::New("Channel").ToLocalChecked(), ctr);
  constructor = new Callback(ctr);
}

bool Channel::HasInstance(Local<Value> val) {
  HandleScope scope;
  return Nan::New(fun_tpl)->HasInstance(val);
}

grpc_channel *Channel::GetWrappedChannel() { return this->wrapped_channel; }

NAN_METHOD(Channel::New) {
  if (info.IsConstructCall()) {
    if (!info[0]->IsString()) {
      return Nan::ThrowTypeError(
          "Channel expects a string, a credential and an object");
    }
    grpc_channel *wrapped_channel;
    // Owned by the Channel object
    Utf8String host(info[0]);
    grpc_channel_credentials *creds;
    if (!ChannelCredentials::HasInstance(info[1])) {
      return Nan::ThrowTypeError(
          "Channel's second argument must be a ChannelCredentials");
    }
    ChannelCredentials *creds_object = ObjectWrap::Unwrap<ChannelCredentials>(
        Nan::To<Object>(info[1]).ToLocalChecked());
    creds = creds_object->GetWrappedCredentials();
    grpc_channel_args *channel_args_ptr = NULL;
    if (!ParseChannelArgs(info[2], &channel_args_ptr)) {
      DeallocateChannelArgs(channel_args_ptr);
      return Nan::ThrowTypeError(
          "Channel options must be an object with "
          "string keys and integer or string values");
    }
    if (creds == NULL) {
      wrapped_channel =
          grpc_insecure_channel_create(*host, channel_args_ptr, NULL);
    } else {
      wrapped_channel =
          grpc_secure_channel_create(creds, *host, channel_args_ptr, NULL);
    }
    DeallocateChannelArgs(channel_args_ptr);
    Channel *channel = new Channel(wrapped_channel);
    channel->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
    return;
  } else {
    const int argc = 3;
    Local<Value> argv[argc] = {info[0], info[1], info[2]};
    MaybeLocal<Object> maybe_instance =
        Nan::NewInstance(constructor->GetFunction(), argc, argv);
    if (maybe_instance.IsEmpty()) {
      // There's probably a pending exception
      return;
    } else {
      info.GetReturnValue().Set(maybe_instance.ToLocalChecked());
    }
  }
}

NAN_METHOD(Channel::Close) {
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError("close can only be called on Channel objects");
  }
  Channel *channel = ObjectWrap::Unwrap<Channel>(info.This());
  if (channel->wrapped_channel != NULL) {
    grpc_channel_destroy(channel->wrapped_channel);
    channel->wrapped_channel = NULL;
  }
}

NAN_METHOD(Channel::GetTarget) {
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "getTarget can only be called on Channel objects");
  }
  Channel *channel = ObjectWrap::Unwrap<Channel>(info.This());
  if (channel->wrapped_channel == NULL) {
    return Nan::ThrowError(
        "Cannot call getTarget on a closed Channel");
  }
  info.GetReturnValue().Set(
      Nan::New(grpc_channel_get_target(channel->wrapped_channel))
          .ToLocalChecked());
}

NAN_METHOD(Channel::GetConnectivityState) {
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "getConnectivityState can only be called on Channel objects");
  }
  Channel *channel = ObjectWrap::Unwrap<Channel>(info.This());
  if (channel->wrapped_channel == NULL) {
    return Nan::ThrowError(
        "Cannot call getConnectivityState on a closed Channel");
  }
  int try_to_connect = (int)info[0]->StrictEquals(Nan::True());
  info.GetReturnValue().Set(grpc_channel_check_connectivity_state(
      channel->wrapped_channel, try_to_connect));
}

NAN_METHOD(Channel::WatchConnectivityState) {
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "watchConnectivityState can only be called on Channel objects");
  }
  if (!info[0]->IsUint32()) {
    return Nan::ThrowTypeError(
        "watchConnectivityState's first argument must be a channel state");
  }
  if (!(info[1]->IsNumber() || info[1]->IsDate())) {
    return Nan::ThrowTypeError(
        "watchConnectivityState's second argument must be a date or a number");
  }
  if (!info[2]->IsFunction()) {
    return Nan::ThrowTypeError(
        "watchConnectivityState's third argument must be a callback");
  }
  Channel *channel = ObjectWrap::Unwrap<Channel>(info.This());
  if (channel->wrapped_channel == NULL) {
    return Nan::ThrowError(
        "Cannot call watchConnectivityState on a closed Channel");
  }
  grpc_connectivity_state last_state = static_cast<grpc_connectivity_state>(
      Nan::To<uint32_t>(info[0]).FromJust());
  double deadline = Nan::To<double>(info[1]).FromJust();
  Local<Function> callback_func = info[2].As<Function>();
  Nan::Callback *callback = new Callback(callback_func);
  unique_ptr<OpVec> ops(new OpVec());
  grpc_channel_watch_connectivity_state(
      channel->wrapped_channel, last_state, MillisecondsToTimespec(deadline),
      GetCompletionQueue(),
      new struct tag(callback, ops.release(), NULL, Nan::Null()));
  CompletionQueueNext();
}

NAN_METHOD(Channel::CreateCall) {
  /* Arguments:
   * 0: Method
   * 1: Deadline
   * 2: host
   * 3: parent Call
   * 4: propagation flags
   */
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "createCall can only be called on Channel objects");
  }
  if (!info[0]->IsString()){
    return Nan::ThrowTypeError("createCall's first argument must be a string");
  }
  if (!(info[1]->IsNumber() || info[1]->IsDate())) {
    return Nan::ThrowTypeError(
      "createcall's second argument must be a date or a number");
  }
  // These arguments are at the end because they are optional
  grpc_call *parent_call = NULL;
  if (Call::HasInstance(info[3])) {
    Call *parent_obj =
        ObjectWrap::Unwrap<Call>(Nan::To<Object>(info[3]).ToLocalChecked());
    parent_call = parent_obj->GetWrappedCall();
  } else if (!(info[3]->IsUndefined() || info[3]->IsNull())) {
    return Nan::ThrowTypeError(
        "createCall's fourth argument must be another call, if provided");
  }
  uint32_t propagate_flags = GRPC_PROPAGATE_DEFAULTS;
  if (info[4]->IsUint32()) {
    propagate_flags = Nan::To<uint32_t>(info[4]).FromJust();
  } else if (!(info[4]->IsUndefined() || info[4]->IsNull())) {
    return Nan::ThrowTypeError(
        "createCall's fifth argument must be propagate flags, if provided");
  }
  Channel *channel = ObjectWrap::Unwrap<Channel>(info.This());
  grpc_channel *wrapped_channel = channel->GetWrappedChannel();
  if (wrapped_channel == NULL) {
    return Nan::ThrowError("Cannot createCall with a closed Channel");
  }
  grpc_slice method =
      CreateSliceFromString(Nan::To<String>(info[0]).ToLocalChecked());
  double deadline = Nan::To<double>(info[1]).FromJust();
  grpc_call *wrapped_call = NULL;
  if (info[2]->IsString()) {
    grpc_slice *host = new grpc_slice;
    *host =
        CreateSliceFromString(Nan::To<String>(info[2]).ToLocalChecked());
    wrapped_call = grpc_channel_create_call(
        wrapped_channel, parent_call, propagate_flags, GetCompletionQueue(),
        method, host, MillisecondsToTimespec(deadline), NULL);
    delete host;
  } else if (info[2]->IsUndefined() || info[2]->IsNull()) {
    wrapped_call = grpc_channel_create_call(
        wrapped_channel, parent_call, propagate_flags, GetCompletionQueue(),
        method, NULL, MillisecondsToTimespec(deadline), NULL);
  } else {
    return Nan::ThrowTypeError("createCall's third argument must be a string");
  }
  grpc_slice_unref(method);
  info.GetReturnValue().Set(Call::WrapStruct(wrapped_call));
}

}  // namespace node
}  // namespace grpc
