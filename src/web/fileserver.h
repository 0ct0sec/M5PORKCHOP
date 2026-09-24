// DEPRECATED (v0.2.0): compatibility shim for renamed XferServer module.
// Prefer: #include "xfer_server.h" and type XferServer.
#pragma once

#include "xfer_server.h"

using FileServer = XferServer;
using FileServerState = XferServerState;

