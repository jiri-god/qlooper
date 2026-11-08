# SPDX-FileCopyrightText: 2026 Kushview, LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set(ELEMENT_RES
    fonts/Roboto-Regular.ttf
    images/icon-template.png
    images/icon.png
    images/power_48x48.png
    acknowledgements.txt
    developers.txt
    sample-pad/acoustic_kick_close.flac
    sample-pad/acoustic_kick_soft.flac
    sample-pad/acoustic_kick_room.flac
    sample-pad/acoustic_tom_low.flac
    sample-pad/acoustic_tom_high.flac
    sample-pad/electronic_808.flac
    sample-pad/electronic_909.flac
    sample-pad/electronic_deephouse.flac
    sample-pad/acoustic_hihat_closed.flac
    sample-pad/acoustic_hihat_pedal.flac
    sample-pad/acoustic_crossstick.flac
    sample-pad/electronic_deephouse_varied_1.flac
    sample-pad/electronic_deephouse_varied_2.flac
    sample-pad/electronic_experimental_classic.flac
    sample-pad/electronic_experimental_varied_1.flac
)
juce_add_binary_data(element-binarydata 
    HEADER_NAME binarydata.hpp 
    NAMESPACE element::res
    SOURCES ${ELEMENT_RES})

# Linux desktop file and icon installation
if(UNIX AND NOT APPLE)
    # Configure desktop file with CMake variables
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/element.desktop.in
        ${CMAKE_CURRENT_BINARY_DIR}/element.desktop
        @ONLY
    )

    # Install desktop file
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/element.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    # Install icon
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/images/icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME element.png
    )
endif()
