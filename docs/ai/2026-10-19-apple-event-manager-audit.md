# Apple Event Manager audit and implementation

Date: 2026-10-19

Source of truth: `docs/api-specs/apple-event-mgr-spec.md`, cross-checked against
*Inside Macintosh Volume VI* (chapter 6) and *Inside Macintosh: Interapplication
Communication* (chapters 3–6).

## Audit result

The pre-existing `src/appleevent` implementation covered the descriptor and list
basics, the three zone tables, and some interaction calls, but several ABI and
semantic gaps were found.

### ABI / call interface

| Finding | Impact |
|---|---|
| `AEManagerInfo` was declared with one argument, but selector `0x0441` has an 8-byte frame (`keyword`, `result`). | The callee popped 4 bytes instead of 8, corrupting the caller's stack. |
| The private handler-table selectors used the wrong argument order (`table` first, etc.). | HyperCard and any other caller of `0x092E`/`0x0830`–`0x0834` would pass the wrong values. |
| `_AE_hdlr_table_free` (`0x042F`), `_AE_hdlr_install_unique` (`0x0830`) and `_AE_hdlr_get_key` (`0x0834`) did not exist. | Required private selectors were missing. |
| Unknown `_Pack8` selectors aborted the emulator. | The spec requires the catch-all path, and `paramErr` when none is installed. *(Deferred; see remaining deviations.)* |
| `_Pack8` results were written to the caller's result slot but not to `D0`. | Callers that read `D0` got the selector instead of the `OSErr`. *(Deferred; see remaining deviations.)* |

### Descriptor and list layer

| Finding | Impact |
|---|---|
| `AEDisposeDesc` did not null the descriptor. | A second dispose, or an iteration over descriptor arrays, could double-free. |
| `AECreateDesc` always allocated a handle even for `dataSize == 0`. | The documented null descriptor for empty data was not produced. |
| `AESizeOfNthItem`/`AEGetNthPtr` copied the *original* descriptor data even when coercion succeeded. | `AEGet…Ptr` returned the coerced type and size but the uncoerced bytes. The same bug existed in `AEGetParamPtr` and `AEGetAttributePtr`. |
| `AEDeleteItem` never deleted anything (the delete path was gated on `create_p`) and then returned `errAEIllegalIndex` on success. | Lists could not be shrunk. |
| The tail shift used for growing, shrinking, and deleting list items had `memmove` source and destination reversed. | Inserting/deleting items of different sizes corrupted the list. |
| `AECreateList` asserted on a non-zero factoring size. | Factored lists from real applications could abort the emulator. |
| `AEGetArray`/`AEPutArray` were no-ops. | Array-based Apple event traffic was silently lost. |
| `AECreateAppleEvent` ignored `returnID` and `transactionID`. | Replies could not be matched to requests. |
| The event's target descriptor was read as a *keyed* descriptor, so the target size came from the first four bytes of target data. | Values such as a PSN with `highLongOfPSN == -1` produced an enormous attribute offset and crashed. |
| The 2-byte gap before a new Apple event's attribute/parameter section was not handled. | Adding the first attribute/parameter could abort or corrupt memory. |

### Handler tables and dispatch

| Finding | Impact |
|---|---|
| Installing the same key with a different handler appended a duplicate instead of replacing it. | Handlers could not be chained/replaced as documented. |
| Event lookup did no wildcard matching and had no specificity ordering. | `typeWildCard` handlers and "more specific wins" dispatch did not work. |
| `AEProcessAppleEvent` consulted only the event table (application, then system), never the special `keyPreDispatch` tables, and returned `errAEHandlerNotFound` instead of `errAEEventNotHandled` when no handler existed. | The required five-step dispatch algorithm was not implemented. |
| A handler returning `errAEEventNotHandled` did not cause the search to continue. | Fallback handlers could not chain. |
| `fromTypeIsDesc` was not stored or returned by the coercion table. | Descriptor-form coercion handlers were invoked as pointer-form handlers and vice versa. |
| `AERemoveEventHandler`/`AERemoveCoercionHandler`/`AERemoveSpecialHandler` ignored the handler argument. | Removing the wrong handler silently removed the installed one. |
| `AEInstallSpecialHandler` accepted any keyword. | It must reject unknown keywords with `errAENotASpecialFunction` (`-1714`). |
| `AEGetEventHandler` used the dispatch lookup. | IM requires an exact (including exact wildcard) match for `AEGetEventHandler`. |

### Creating, sending, current event, interaction

| Finding | Impact |
|---|---|
| `AEManagerInfo` ignored its keyword. | `keyAEVersion`/`keyAERecorderCount` returned nothing useful. |
| `AESend` rejected `kAEWaitReply`/`kAEQueueReply` for same-process targets and ignored the reply mode. | Common send paths failed or produced no reply. |
| `AEGetTheCurrentEvent`/`AESetTheCurrentEvent` did nothing. | Handlers could not inspect the current event. |
| `AESuspendTheCurrentEvent`/`AEResumeTheCurrentEvent` did nothing (resume did not call a substitute dispatcher). | The documented suspend/resume interface was absent. |
| `AEInteractWithUser` always returned `noErr`. | It ignored the application's interaction level. |
| Object-support selectors returned `noErr` while doing nothing. | Callers believed a token/accessor had been created. |

### Constants

Many documented result codes, descriptor types, attribute/parameter keywords,
event classes, callback flags, key forms, and manager-info keywords were missing
from `multiversal/defs/AppleEvents.yaml`.

## Implementation

- `multiversal/defs/AppleEvents.yaml`: added the missing error codes, types,
  keywords, event classes, key forms and `AEResolve` flags; corrected the
  `AEManagerInfo` and private-selector signatures; added the three missing
  private selectors; changed `AEArrayType` to a 16-bit integer.
- `src/appleevent/AE_hdlr.cpp`: rewrote the handler-table layer (replace vs.
  unique, absent-delete success, key enumeration, zone/imports arguments),
  implemented wildcard dispatch specificity, the special-handler keyword
  validation, and the `fromTypeIsDesc` flag.
- `src/appleevent/AE_desc.cpp`: fixed descriptor disposal/null handling, index
  handling (`0`, `-1`), `memmove` direction, list deletion, event layout and
  attribute/parameter appends, `AECreateAppleEvent` return/transaction
  attributes, and implemented `AEGetArray`/`AEPutArray`.
- `src/appleevent/AE.cpp`: implemented the five-step dispatch algorithm, special
  pre-dispatch handlers, synchronous same-process `kAEWaitReply`, queued
  replies, current-event state, interaction-level checks, `AEManagerInfo`, and
  the `fromTypeIsDesc` paths.
- `src/appleevent/AE_coercion.cpp`: implemented the default coercions from
  IM-IAC table 4-1 (text/numeric, numeric/numeric, type/enumerated/boolean,
  alias→FSS, any→list, single-item list→item).
- `src/base/traps.impl.h`: intentionally unchanged.  The generic dispatcher still
  aborts (or breaks to the debugger) on unknown selectors and only writes Pack 8
  results to the caller's result slot.  Adding the catch-all path and the `D0`
  mirror inside the innermost dispatcher loop is deliberately deferred.
- `tests/appleevents.cpp`: added regression tests for descriptor disposal,
  coercion-copying, list deletion, event attributes, factoring arguments,
  default coercion, handler replacement/wildcards, private table semantics,
  arrays, and `AEManagerInfo`.

## Remaining intentional deviations

- The generic trap dispatcher still aborts on an unknown selector.  The spec's
  catch-all forwarding (and `paramErr` when no catch-all is installed) is not
  implemented; that error-handling feature is deferred.
- `_Pack8` results are only written to the two-byte result slot above the
  arguments.  They are not mirrored into `D0`; callers that read `D0` are not
  supported yet.

- Executor has no Object Support Library. The object-support selectors remain
  callable but return `paramErr`, which is the observable behaviour of the
  catch-all path when no OSL handler is installed. There is no public selector
  that installs a catch-all, so no such hook is exposed.
- `AESend` supports same-process targets only. `kAEWaitReply` is processed
  synchronously (no yielding); `kAEQueueReply` posts a high-level event and the
  reply is posted by `AEProcessAppleEvent`.
- `AESuspendTheCurrentEvent` is a no-op and `AEResumeTheCurrentEvent` calls the
  supplied dispatcher directly; there is no actual suspension of an in-progress
  handler.
- `AEInteractWithUser` enforces the interaction level but does not post a
  notification request or switch layers.
- Factoring arguments to `AECreateList` are accepted and ignored. Lists always
  store complete item data; this is invisible to callers but means no space
  optimisation for packed arrays.
- The private handler tables accept the observed `sizes == 0x00080008` only.
  Other key/value sizes are not used by the manager or by any observed caller.
- `_AE_hdlr_get_key` treats `index` as 1-based. The spec does not define the
  base and no observed caller uses this selector.
