//  ZMTP protocol class

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.*/

#ifndef __ZMTP_HANDSHAKE_H_INCLUDED__
#define __ZMTP_HANDSHAKE_H_INCLUDED__

#include "codec.h"

typedef struct zmtp_handshake zmtp_handshake_t;

zmtp_handshake_t *
    zmtp_handshake_new ();

codec_t *
    zmtp_handshake_new_codec ();

#endif
