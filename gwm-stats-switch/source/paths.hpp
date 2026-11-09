#pragma once

/// Where the module keeps its files on the card. Named after the site, like
/// the module itself in Sysmodules: a person sees one name everywhere.
#define GWM_CONFIG_DIR "sdmc:/config/gowithme.club"

/// The folder the module lived in before 1.1.0. Still read, never written:
/// a card set up for an older build keeps working without a move.
#define GWM_LEGACY_CONFIG_DIR "sdmc:/config/gwm-stats-switch"
