// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/** Settings in this file can only be configured with the CPP preprocessor.
    Modify them to change things like app name.
 */

// clang-format off
#ifndef EL_APP_AUTHOR
 #define EL_APP_AUTHOR "princ.biz"
#endif

#ifndef EL_APP_NAME
 #define EL_APP_NAME "QLooper"
#endif

#ifndef EL_APP_URL
 #define EL_APP_URL "https://qlooper.princ.biz"
#endif

#define EL_APP_DATA_SUBDIR EL_APP_AUTHOR "/" EL_APP_NAME
// clang-format on

namespace element {

}
