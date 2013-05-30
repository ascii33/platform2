// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef P2P_SERVER_HTTP_SERVER_H__
#define P2P_SERVER_HTTP_SERVER_H__

#include <string>

#include <base/basictypes.h>
#include <base/callback.h>
#include <base/file_path.h>

namespace p2p {

namespace server {

// Interface for starting/stopping a HTTP server and getting feedback
// about the number of connected HTTP clients.
class HttpServer {
 public:
  // Called when number of connections changes
  typedef base::Callback<void(int num_connections)> NumConnectionsCallback;

  virtual ~HttpServer() {}

  // Statrs the HTTP server.
  virtual bool Start() = 0;

  // Stops the HTTP server.
  virtual bool Stop() = 0;

  // Returns true the HTTP has been started.
  virtual bool IsRunning() = 0;

  // Sets the callback function used for reporting number of connections.
  // In order to receive callbacks, you need to run the default
  // GLib main-loop.
  virtual void SetNumConnectionsCallback(NumConnectionsCallback callback) = 0;

  // Creates and initializes a suitable HttpServer instance for
  // serving files from |root_dir| on the TCP port given by |port|.
  // Note that the server will not initially be running; use the
  // Start() method to start it.
  static HttpServer* Construct(const base::FilePath& root_dir, uint16 port);
};

}  // namespace server

}  // namespace p2p

#endif  // P2P_SERVER_HTTP_SERVER_H__
