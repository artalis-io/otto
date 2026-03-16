#ifndef ARBOR_H
#define ARBOR_H

#include "ar_alns.h"
#include "ar_operators.h"
#include "ar_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARBOR_VERSION_MAJOR 0
#define ARBOR_VERSION_MINOR 1
#define ARBOR_VERSION_PATCH 0

const char *ar_version(void);

#ifdef __cplusplus
}
#endif

#endif /* ARBOR_H */
