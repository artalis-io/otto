/*
 * sh_transport_internal.h - Stream plumbing shared between transports.
 *
 * Private to shared/src and to transport drivers (sh_transport_direct,
 * sh_keelserver). Handlers never see this: they get an opaque ShApiStream and
 * the two functions in sh_api.h.
 *
 * This is the one place a transport-shaped vtable is allowed, because it is a
 * private implementation detail rather than the interface modules program
 * against. The public ShTransport stays at serve/stop.
 */
#ifndef SH_TRANSPORT_INTERNAL_H
#define SH_TRANSPORT_INTERNAL_H

#include <stddef.h>
#include "sh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int (*send)  (ShApiStream *s, const char *event,
                  const void *data, size_t len);
    int (*closed)(const ShApiStream *s);
} ShApiStreamVTable;

/*
 * Base of every concrete stream. A transport embeds this as its first member
 * and downcasts, so `vt` is always at offset 0.
 */
struct ShApiStream {
    const ShApiStreamVTable *vt;
};

#ifdef __cplusplus
}
#endif

#endif /* SH_TRANSPORT_INTERNAL_H */
