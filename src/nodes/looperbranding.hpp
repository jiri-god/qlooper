// SPDX-FileCopyrightText: 2026 QLooper contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/tags.hpp>
#include <juce_data_structures/juce_data_structures.h>
namespace element {
// Used only when inserting a new instance. Never rename loaded session nodes.
inline juce::String nextQLooperName (const juce::ValueTree& nodes)
{
    for (int number = 1; ; ++number)
    {
        const auto name = juce::String ("QLooper ") + juce::String (number);
        bool used = false;
        for (const auto& node : nodes)
            if (node.getProperty (tags::name).toString() == name) { used = true; break; }
        if (! used) return name;
    }
}
}
