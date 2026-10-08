/* SPDX-License-Identifier: BSD-3-Clause */
// Shared kernel error codes. The house convention holds: 0 (ErrOk) is
// success and negative values name the failure, so `rc != ErrOk` and the
// historical `rc != 0` checks both keep working.
#ifndef ERRORS_H
#define ERRORS_H

#define ErrOk        0 // success
#define ErrIo      (-1) // device refused, bad arguments or floating bus
#define ErrTimeout (-2) // a poll ran out of time before the device answered
#define ErrUnsupported (-3) // the device does not support the requested command
#define ErrNoMem   (-4) // no memory could be provided for the requested mapping or buffer
#define ErrCorrupt (-5) // on-disk structure disagrees with itself (bad range, double free)

#define ErrFault   (-17) // puntero de usuario inválido
#define ErrPerm    (-18)
#define ErrBadHandle (-19)
#define ErrDead    (-20) // el otro extremo cerró
#define ErrAgain   (-21)
#define ErrNoProcess (-22)
#define ErrTooBig  (-23)

#endif
