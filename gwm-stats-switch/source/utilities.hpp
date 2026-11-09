#pragma once

#include <switch.h>

/// Blocks until nifm reports an established internet connection.
void waitForNetworkReady();

typedef struct {
    u64 pid;               ///< process id of the running application
    u64 tid;               ///< program (title) id
    char title_name[513];  ///< NACP title name, 512 + null terminator
} AppInfo;

/// Longest nickname acc returns (0x20 bytes) plus a terminator.
#define USER_NAME_SIZE 0x21

struct UserInfo {
    AccountUid uid;
    char uid_string[33];
    char nickname[USER_NAME_SIZE];
};

/// Fills `info` with the currently running application. Returns 0 on success,
/// a libnx Result otherwise (including when no application is running).
Result get_app_info(AppInfo* info);

/// Reads a title's localized NACP name without allocating its JPEG icon.
bool getTitleName(u64 titleId, char* out, size_t outSize);

/// Opens acc and pdm:qry, and closes them again. Everything below needs them,
/// and nothing below keeps them: a sysmodule that initializes a service at boot
/// holds that session for as long as the console is on, out of a pool the whole
/// system shares. On this console `hid` died with kernel LimitReached
/// (2001-0132) while a game was starting, twice, with these held open — so they
/// are opened for the seconds a profile lookup takes and given back.
bool openUserServices();
void closeUserServices();

/// The user the console has open. This is "last opened", not "playing": a game
/// opens its user some seconds into its own startup, so right after a launch
/// this still names whoever played before. Returns false when the console
/// cannot say (nobody opened since boot, acc unavailable).
bool getCurrentUserUid(AccountUid* out);

/// Nickname of a user. Returns false when the profile cannot be read.
bool getUserName(const AccountUid* uid, char* out, size_t outSize);

/// Stable UID plus its optional local nickname.
bool getUserInfo(const AccountUid* uid, UserInfo* out);

/// Resolves the canonical account event at an exact global PDM entry index.
bool getUserForPlayEventIndex(s32 entryIndex, UserInfo* out);

/// Resolves a user tied to one exact application launch. Account events are
/// authoritative; per-user statistics are the fallback for titles which omit
/// them. Both paths are bounded by launch index and timestamp.
bool getUserForPlayLaunch(u64 titleId, s32 launchIndex, u64 launchTimestamp, UserInfo* out);

/// The user the console's own play journal says opened `titleId` at or after
/// `sinceRawSec` (console clock, uncorrected — that is what pdm stamps with).
/// This is the authority: pdm records the account together with the
/// application, so it answers "whose session is this" rather than "what
/// happened most recently". The entry appears when the game opens its user, a
/// few seconds into startup.
///
/// The time bound is the whole trick. The journal holds every launch the
/// console ever saw, so an unbounded search happily returns whoever played this
/// same game last week.
bool getUserForTitle(u64 titleId, u64 sinceRawSec, AccountUid* out);

/// The single user the console currently has open, when there is exactly one.
/// libnx wraps ListAllUsers but not ListOpenUsers, which is the useful one.
bool getSingleOpenUser(AccountUid* out);

/// The console's only registered profile, when it has exactly one. Not a guess:
/// with a single profile there is nobody else the session could belong to, so
/// this answers immediately instead of waiting out the grace window.
bool getOnlyRegisteredUser(AccountUid* out);

/// The profile named by `defaultUid` in the config, as a canonical 32-digit
/// string. This is the console owner deciding in advance who unattributed
/// launches belong to — homebrew ports and forwarders never open a profile, so
/// pdm records no account for them and every other source comes up empty.
/// The uid is matched against the console's own profile list, never trusted.
bool getConfiguredDefaultUser(const char* canonicalUid, AccountUid* out);

/// Writes what each source thinks, for the log. Diagnostics, not a decision.
void describeUserSources(u64 titleId, u64 sinceRawSec, char* out, size_t outSize);

// Sleep/wake through psc. The whole sysmodule is frozen while the console
// sleeps, so clocks and the system tick are useless for detecting it; psc hands
// us a real event instead. ReadySleep fires *before* the console goes down while
// wifi is still up, which is the only moment we can flush a final heartbeat.
extern PscPmModule g_pscModule;
extern Thread g_pscThread;
extern bool g_pscReady;
extern volatile bool g_pscThreadRun;
extern volatile bool g_wokeFromSleep;
extern volatile bool g_sleepPending;
extern volatile bool g_asleep;
extern UEvent g_pscWakeMain;
extern UEvent g_sleepPrepDone;

void pscThreadFunc(void*);

/// Offset between the console clock and real time, learned from HTTP `Date`
/// headers. The RTC drifts badly on CFW and the ingest server rejects
/// timestamps from the future, so every timestamp we send is corrected.
extern int64_t g_timeOffset;

/// Console clock, uncorrected. Use for measuring intervals, never for sending.
u64 getRawNowSec();

/// Console clock corrected by `g_timeOffset`. Use for everything we send.
u64 getCorrectedNowSec();

/// Recomputes the offset from a server timestamp. A move of a single second is
/// ignored: it is quantization between two whole-second clocks, not drift, and
/// following it duplicated sessions on the server.
void updateTimeOffset(int64_t newOffset);

/// Formats a corrected epoch second as ISO 8601 UTC ("2026-08-07T18:30:05Z").
/// `out` must hold at least 21 bytes.
void formatIso8601(u64 epochSec, char* out, size_t outSize);

/// Reads "YYYY-MM-DDTHH:MM:SSZ" (fractions and offsets are ignored, UTC is
/// assumed) into seconds since the epoch. Returns 0 for anything else.
u64 parseIso8601(const char* text);

/// Formats a play span for a toast, in Russian: "1 ч 23 мин", "45 мин",
/// "< 1 мин". `out` must hold at least 20 bytes.
void formatPlaySpan(int seconds, char* out, size_t outSize);
