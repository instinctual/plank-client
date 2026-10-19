#pragma once

// The Ubuntu Qt experiment must coexist with the qualified Client, including
// its mutable state. Keep the production identity unchanged on every platform.
namespace PlankClientIdentity {
#ifdef PLANK_QT_CLIENT_EXPERIMENT
#ifndef __linux__
#error The Qt session experiment is supported only on Linux
#endif
inline constexpr bool experimental = true;
inline constexpr char applicationName[] = "PLANK Client Qt";
inline constexpr char displayName[] = "PLANK Client Qt";
inline constexpr char desktopId[] = "la.instinctual.Plank.Client.Qt";
inline constexpr char executable[] = "plank-client-qt";
inline constexpr char logDirectory[] = "plank-client-qt/logs";
inline constexpr char configPath[] = "/etc/plank/client-qt.conf";
#else
inline constexpr bool experimental = false;
inline constexpr char applicationName[] = "PLANK";
inline constexpr char displayName[] = "PLANK Client";
inline constexpr char desktopId[] = "la.instinctual.Plank.Client";
inline constexpr char executable[] = "plank-client";
inline constexpr char logDirectory[] = "plank/logs";
inline constexpr char configPath[] = "/etc/plank/client.conf";
#endif
}
