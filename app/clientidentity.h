#pragma once

// Frontend selection must not change bookmarks, trust, policy or log identity.
namespace PlankClientIdentity {
inline constexpr char applicationName[] = "PLANK";
inline constexpr char displayName[] = "PLANK Client";
inline constexpr char desktopId[] = "la.instinctual.Plank.Client";
inline constexpr char executable[] = "plank-client";
inline constexpr char logDirectory[] = "plank/logs";
inline constexpr char configPath[] = "/etc/plank/client.conf";
}
