// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "plugin.h"

#include <stdio.h>
#include <string.h>
#include <resolv.h>

#include "ppapi/cpp/module.h"

#include "json/reader.h"
#include "json/writer.h"

#include "file_system.h"

const char kMessageNameAttr[] = "name";
const char kMessageArgumentsAttr[] = "arguments";

// These are C++ the method names as JavaScript sees them.
const char kStartSessionMethodId[] = "startSession";
const char kOnOpenFileMethodId[] = "onOpenFile";
const char kOnOpenSocketMethodId[] = "onOpenSocket";
const char kOnReadMethodId[] = "onRead";
const char kOnWriteAcknowledgeMethodId[] = "onWriteAcknowledge";
const char kOnCloseMethodId[] = "onClose";
const char kOnResizeMethodId[] = "onResize";

// Known startSession attributes.
const char kTerminalWidthAttr[] = "terminalWidth";
const char kTerminalHeightAttr[] = "terminalHeight";
const char kUseJsSocketAttr[] = "useJsSocket";
const char kEnvironmentAttr[] = "environment";
const char kWriteWindowAttr[] = "writeWindow";

// These are JavaScript method names as C++ code sees them.
const char kPrintLogMethodId[] = "printLog";
const char kExitMethodId[] = "exit";
const char kOpenFileMethodId[] = "openFile";
const char kOpenSocketMethodId[] = "openSocket";
const char kWriteMethodId[] = "write";
const char kReadMethodId[] = "read";
const char kCloseMethodId[] = "close";

const size_t kDefaultWriteWindow = 64 * 1024;

//------------------------------------------------------------------------------

PluginInstance* PluginInstance::instance_ = NULL;

PluginInstance::PluginInstance(PP_Instance instance)
    : pp::Instance(instance),
      core_(pp::Module::Get()->core()),
      plugin_thread_(NULL),
      factory_(this),
      file_system_(this, this) {
  instance_ = this;
}

PluginInstance::~PluginInstance() {
  instance_ = NULL;
}

void PluginInstance::HandleMessage(const pp::Var& message_data) {
  if (message_data.is_string()) {
    Json::Value root;
    if (Json::Reader().parse(message_data.AsString(), root) &&
        root.isObject()) {
      std::string function = root[kMessageNameAttr].asString();
      const Json::Value& args = root[kMessageArgumentsAttr];
      if (!function.empty() && args.isArray())
        Invoke(function, args);
    }
  }
}

void PluginInstance::Invoke(const std::string& function,
                            const Json::Value& args) {
  if (function == kStartSessionMethodId) {
    StartSession(args);
  } else if (function == kOnOpenFileMethodId ||
             function == kOnOpenSocketMethodId) {
    OnOpen(args);
  } else if (function == kOnReadMethodId) {
    OnRead(args);
  } else if (function == kOnWriteAcknowledgeMethodId) {
    OnWriteAcknowledge(args);
  } else if (function == kOnCloseMethodId) {
    OnClose(args);
  } else if (function == kOnResizeMethodId) {
    OnResize(args);
  }
}

void PluginInstance::InvokeJS(const std::string& function,
                              const Json::Value& args) {
  Json::Value root;
  root[kMessageNameAttr] = Json::Value(function);
  root[kMessageArgumentsAttr] = args;
  Json::FastWriter writer;
  std::string json = writer.write(root);
  PostMessage(pp::Var(json));
}

void PluginInstance::PrintLogImpl(int32_t result, const std::string& msg) {
  Json::Value call_args(Json::arrayValue);
  call_args.append(msg);
  InvokeJS(kPrintLogMethodId, call_args);
}

void PluginInstance::PrintLog(const std::string& msg) {
  core_->CallOnMainThread(0, factory_.NewCallback(
      &PluginInstance::PrintLogImpl, msg));
}

void PluginInstance::SessionClosedImpl(int32_t result, const int& error) {
  Json::Value call_args(Json::arrayValue);
  call_args.append(error);
  InvokeJS(kExitMethodId, call_args);
}

void PluginInstance::SessionClosed(int error) {
  core_->CallOnMainThread(0, factory_.NewCallback(
      &PluginInstance::SessionClosedImpl, error));
  plugin_thread_ = NULL;
}

bool PluginInstance::OpenFile(int fd, const char* name, int mode,
                              InputInterface* stream) {
  if (name) {
    Json::Value call_args(Json::arrayValue);
    call_args.append(fd);
    call_args.append(std::string(name));
    call_args.append(mode);
    InvokeJS(kOpenFileMethodId, call_args);
  }
  assert(streams_.find(fd) == streams_.end());
  streams_[fd] = stream;
  return true;
}

bool PluginInstance::OpenSocket(int fd, const char* host, uint16_t port,
                                InputInterface* stream) {
  Json::Value call_args(Json::arrayValue);
  call_args.append(fd);
  call_args.append(std::string(host));
  call_args.append(port);
  InvokeJS(kOpenSocketMethodId, call_args);
  assert(streams_.find(fd) == streams_.end());
  streams_[fd] = stream;
  return true;
}

bool PluginInstance::Write(int fd, const char* data, size_t size) {
  const size_t kMaxWriteSize = 24*1024;
  std::vector<char> buf(kMaxWriteSize * 4 / 3 + 4);
  size_t start = 0;
  while(start < size) {
    Json::Value call_args(Json::arrayValue);
    call_args.append(fd);
    size_t chunk_size = ((size - start) <= kMaxWriteSize) ? (size - start)
                                                          : kMaxWriteSize;
    int res = b64_ntop((const unsigned char*)data + start, chunk_size,
                       &buf[0], buf.size());
    if (res <= 0) {
      assert(res > 0);
      return false;
    }
    call_args.append(&buf[0]);
    start += chunk_size;
    InvokeJS(kWriteMethodId, call_args);
  }
  return true;
}

bool PluginInstance::Read(int fd, size_t size) {
  Json::Value call_args(Json::arrayValue);
  call_args.append(fd);
  call_args.append(size);
  InvokeJS(kReadMethodId, call_args);
  return true;
}

bool PluginInstance::Close(int fd) {
  Json::Value call_args(Json::arrayValue);
  call_args.append(fd);
  InvokeJS(kCloseMethodId, call_args);
  return true;
}

size_t PluginInstance::GetWriteWindow() {
  if (session_args_.isMember(kWriteWindowAttr) &&
      session_args_[kWriteWindowAttr].isNumeric()) {
    return session_args_[kWriteWindowAttr].asInt();
  }
  return kDefaultWriteWindow;
}

void* PluginInstance::SessionThread(void* arg) {
  PluginInstance* instance = static_cast<PluginInstance*>(arg);
  instance->SessionThreadImpl();
  return NULL;
}

const char* PluginInstance::GetEnvironmentVariable(const char* name) {
  if (session_args_.isMember(kEnvironmentAttr) &&
      session_args_[kEnvironmentAttr].isObject() &&
      session_args_[kEnvironmentAttr].isMember(name)) {
    return session_args_[kEnvironmentAttr][name].asCString();
  }

  return NULL;
}

void PluginInstance::StartSession(const Json::Value& args) {
  if (args.size() == 1 && args[(size_t)0].isObject() && !plugin_thread_) {
    session_args_ = args[(size_t)0];
  if (session_args_.isMember(kTerminalWidthAttr) &&
      session_args_[kTerminalWidthAttr].isNumeric() &&
      session_args_.isMember(kTerminalHeightAttr) &&
      session_args_[kTerminalHeightAttr].isNumeric()) {
    file_system_.SetTerminalSize(session_args_[kTerminalWidthAttr].asInt(),
                                 session_args_[kTerminalHeightAttr].asInt());
  }
  if (session_args_.isMember(kUseJsSocketAttr) &&
      session_args_[kUseJsSocketAttr].isBool()) {
    file_system_.UseJsSocket(session_args_[kUseJsSocketAttr].asBool());
  }
  if (pthread_create(&plugin_thread_, NULL,
                     &PluginInstance::SessionThread, this)) {
      SessionClosedImpl(0, -1);
    }
  } else {
    PrintLogImpl(0, "startSession: invalid arguments\n");
  }
}

void PluginInstance::OnOpen(const Json::Value& args) {
  const Json::Value& fd = args[(size_t)0];
  const Json::Value& result = args[(size_t)1];
  if (fd.isNumeric() && result.isBool()) {
    InputStreams::iterator it = streams_.find(fd.asInt());
    if (it != streams_.end()) {
      it->second->OnOpen(result.asBool());
      if (!result.asBool())
        streams_.erase(it);
    } else {
      PrintLogImpl(0, "onOpen: for unknown file descriptor\n");
    }
  } else {
    PrintLogImpl(0, "onOpen: invalid arguments\n");
  }
}

void PluginInstance::OnRead(const Json::Value& args) {
  const Json::Value& fd = args[(size_t)0];
  const Json::Value& data = args[(size_t)1];
  if (fd.isNumeric() && data.isString()) {
    InputStreams::iterator it = streams_.find(fd.asInt());
    if (it != streams_.end()) {
      const std::string& str = data.asString();
      std::vector<char> buf(str.size() * 3 / 4);
      int res = b64_pton(str.c_str(), (unsigned char*)&buf[0], buf.size());
      assert(res >= 0);
      it->second->OnRead(&buf[0], res);
    } else {
      PrintLogImpl(0, "onRead: for unknown file descriptor\n");
    }
  } else {
    PrintLogImpl(0, "onRead: invalid arguments\n");
  }
}

void PluginInstance::OnWriteAcknowledge(const Json::Value& args) {
  const Json::Value& fd = args[(size_t)0];
  const Json::Value& count = args[(size_t)1];
  if (fd.isNumeric() && count.isNumeric()) {
    InputStreams::iterator it = streams_.find(fd.asInt());
    if (it != streams_.end()) {
      // TODO(dpolukhin): UInt here is only 32-bit, current version of json lib
      // don't support 64-bit integer numbers.
      it->second->OnWriteAcknowledge(count.asUInt());
    } else {
      PrintLogImpl(0, "onWriteAcknowledge: for unknown file descriptor\n");
    }
  } else {
    PrintLogImpl(0, "onWriteAcknowledge: invalid arguments\n");
  }
}

void PluginInstance::OnClose(const Json::Value& args) {
  const Json::Value& fd = args[(size_t)0];
  InputStreams::iterator it = streams_.find(fd.asInt());
  if (it != streams_.end()) {
    it->second->OnClose();
    streams_.erase(it);
  } else {
    PrintLogImpl(0, "onClose: for unknown file descriptor\n");
  }
}

void PluginInstance::OnResize(const Json::Value& args) {
  file_system_.SetTerminalSize(args[(size_t)0].asInt(),
                               args[(size_t)1].asInt());
}
