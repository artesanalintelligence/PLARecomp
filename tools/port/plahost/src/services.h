// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The services the game still asks for over IPC once the nnshim and nvnvk mods serve the rest
// (measured with suyu's SUYU_IPC_STATS: docs/port/HOST.md).

#pragma once

#include <memory>
#include <string>

namespace plahost::ipc {
class Service;
}

namespace plahost::services {

std::shared_ptr<ipc::Service> MakeNvdrv(const std::string& name);

/// Lowers the applet message event (the app is in focus); called at startup.
void InitApplet();
/// Queues an applet message (e.g. 4 = Exit) and signals the event.
void PushAppletMessage(unsigned message);

} // namespace plahost::services
