// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace element {
struct SamplePadBankEntry { const char* name; const char* resource; };
inline constexpr SamplePadBankEntry samplePadBank[] {
    {"Acoustic kick - close", "acoustic_kick_close_flac"},
    {"Acoustic kick - soft", "acoustic_kick_soft_flac"},
    {"Acoustic kick - room", "acoustic_kick_room_flac"},
    {"Acoustic tom - low", "acoustic_tom_low_flac"},
    {"Acoustic tom - high", "acoustic_tom_high_flac"},
    {"Electronic kick - 808 style", "electronic_808_flac"},
    {"Electronic kick - 909 style", "electronic_909_flac"},
    {"Electronic kick - deep house", "electronic_deephouse_flac"},
    {"Acoustic hi-hat - closed", "acoustic_hihat_closed_flac"},
    {"Acoustic hi-hat - pedal chick", "acoustic_hihat_pedal_flac"},
    {"Acoustic snare - cross-stick", "acoustic_crossstick_flac"},
    {"Electronic kick - deep house variation 1", "electronic_deephouse_varied_1_flac"},
    {"Electronic kick - deep house variation 2", "electronic_deephouse_varied_2_flac"},
    {"Electronic kick - experimental", "electronic_experimental_classic_flac"},
    {"Electronic kick - experimental variation 1", "electronic_experimental_varied_1_flac"},
};
}
