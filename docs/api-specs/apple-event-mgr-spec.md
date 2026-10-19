# AppleEvent Manager (PACK 8) — implementation specification

Sources are cited inline:

| tag | source |
|---|---|
| (IM-VI) | *Inside Macintosh Volume VI* (1991), chapter 6, "The Apple Event Manager" |
| (IM-IAC) | *Inside Macintosh: Interapplication Communication* (1993), chapters 3–6 |
| (AE-HDR) | Apple's Universal Interfaces headers (`AppleEvents.h`, `AEDataModel.h`, `AEInteraction.h`, `AEObjects.h`, `AERegistry.h`) |
| (HC) | observed HyperCard behaviour |
| (PACK) | disassembly of `PACK` 8 from the System 7.5.3 `System` file |
| (EXEC) | pre-existing Executor code — corroborating only |

Names, types and constants not documented by Apple are marked **_(undoc)_**.

## 1. Introduction

The AppleEvent Manager is not a family of traps. It is a single trap, `_Pack8`
(`0xA816`), with a **16-bit selector in `D0.W`**. Every manager routine has its own
selector; Apple's own headers encode the call as
`move.w #<selector>,d0; _Pack8` (AE-HDR). A replacement must therefore dispatch on the
selector word, and the selectors below are part of the ABI, not an implementation
detail.

Two things about the selector are worth stating because they are load-bearing:

* **Low byte = entry index, high byte = half the argument frame size**, i.e.
  `selector = (argBytes/2) << 8 | index` (PACK). The high byte is the only thing that
  tells the manager how many bytes to pop.
* Selectors whose low byte is **not** in the manager's table are not errors: they are
  forwarded to a dynamically-installed catch-all handler; if there is none the response
  is `paramErr` (−50), i.e. the *object-support* selectors of §2.9 are only usable once
  an object-support implementation has installed its catch-all (PACK). HyperCard's
  object-accessor traffic goes through exactly this path and tolerates the `paramErr`
  (HC).

Calling convention (PACK):

* arguments on the stack, right-to-left, `pascal` style — the **callee pops them**;
* the `OSErr` result is returned as a 16-bit value in **`D0`** *and* written to a 2-byte
  slot the caller reserves immediately **above** its arguments. Callers exist that read
  either, so a replacement must do both.

The manager keeps **per-zone** state: an *application* zone and a *system* zone. The
records hang off low memory (PACK): the pointer at `ExpandMem` (`0x2B6`) has the
application record at `+0x154` and the system record at `+0x17C`; each is a handle to a
record whose first fields are the dispatch tables:

| offset | table |
|---:|---|
| +0 | event handler table |
| +4 | coercion handler table |
| +8 | special handler table |

**HyperCard dereferences `(*zoneRecord)+8`** and writes a table handle into it (HC), so
the location *and* the field order above are observable interface, not private state.
Everything about how a table is organised internally is not observable and is
deliberately left unspecified here (see §2.10).

Where an event is dispatched: the **application** zone's tables are consulted before the
**system** zone's (IM-IAC).

## 2. Functions

Every routine below is entered with `move.w #<selector>,d0; _Pack8`. `argBytes` is the
frame size implied by the selector (PACK). Signatures use Apple's names (IM-IAC,
AE-HDR).

### 2.1 Descriptor records

| selector | argBytes | routine |
|---|---:|---|
| `0x0825` | 16 | `AECreateDesc(typeCode, dataPtr, dataSize, result)` |
| `0x0204` | 4 | `AEDisposeDesc(theAEDesc)` |
| `0x0405` | 8 | `AEDuplicateDesc(theAEDesc, result)` |
| `0x0A02` | 20 | `AECoercePtr(typeCode, dataPtr, dataSize, toType, result)` |
| `0x0603` | 12 | `AECoerceDesc(theAEDesc, toType, result)` |

```
OSErr AECreateDesc (DescType typeCode; Ptr dataPtr; Size dataSize; AEDesc *result);
OSErr AEDisposeDesc(AEDesc *theAEDesc);
OSErr AEDuplicateDesc(const AEDesc *theAEDesc; AEDesc *result);
OSErr AECoercePtr  (DescType typeCode; Ptr dataPtr; Size dataSize;
                    DescType toType; AEDesc *result);
OSErr AECoerceDesc (const AEDesc *theAEDesc; DescType toType; AEDesc *result);
```

* `AECreateDesc` copies `dataSize` bytes into a new handle; `result.dataHandle` may be
  NULL when `dataSize` is 0 (IM-IAC).
* `AEDisposeDesc` disposes the `dataHandle` and nulls the descriptor; it is safe on a
  descriptor whose handle is already NULL (IM-IAC).
* `AECoerceDesc`/`AECoercePtr` apply the coercion handler for `(source type, toType)`.
  If no handler fits, the result is `errAECoercionFail` (−1700) (IM-IAC).

### 2.2 Descriptor lists and AE records

| selector | argBytes | routine |
|---|---:|---|
| `0x0706` | 14 | `AECreateList(factoringPtr, factoredSize, isRecord, resultList)` |
| `0x0407` | 8 | `AECountItems(theAEDescList, theCount)` |
| `0x0A08` | 20 | `AEPutPtr(theAEDescList, index, typeCode, dataPtr, dataSize)` |
| `0x0609` | 12 | `AEPutDesc(theAEDescList, index, theAEDesc)` |
| `0x100A` | 32 | `AEGetNthPtr(theAEDescList, index, desiredType, theAEKeyword, typeCode, dataPtr, maximumSize, actualSize)` |
| `0x0A0B` | 20 | `AEGetNthDesc(theAEDescList, index, desiredType, theAEKeyword, result)` |
| `0x082A` | 16 | `AESizeOfNthItem(theAEDescList, index, typeCode, dataSize)` |
| `0x0D0C` | 26 | `AEGetArray(theAEDescList, arrayType, arrayPtr, maximumSize, itemType, itemSize, itemCount)` |
| `0x0B0D` | 22 | `AEPutArray(theAEDescList, arrayType, arrayPtr, itemType, itemSize, itemCount)` |
| `0x040E` | 8 | `AEDeleteItem(theAEDescList, index)` |

```
OSErr AECreateList (Ptr factoringPtr; Size factoredSize; Boolean isRecord;
                    AEDescList *resultList);
OSErr AECountItems  (const AEDescList *theAEDescList; long *theCount);
OSErr AEPutPtr      (AEDescList *theAEDescList; long index; DescType typeCode;
                     const void *dataPtr; Size dataSize);
OSErr AEPutDesc     (AEDescList *theAEDescList; long index; const AEDesc *theAEDesc);
OSErr AEGetNthPtr   (const AEDescList *theAEDescList; long index; DescType desiredType;
                     AEKeyword *theAEKeyword; DescType *typeCode; Ptr dataPtr;
                     Size maximumSize; Size *actualSize);
OSErr AEGetNthDesc  (const AEDescList *theAEDescList; long index; DescType desiredType;
                     AEKeyword *theAEKeyword; AEDesc *result);
OSErr AESizeOfNthItem(const AEDescList *theAEDescList; long index;
                      DescType *typeCode; Size *dataSize);
OSErr AEGetArray    (const AEDescList *theAEDescList; AEArrayType arrayType;
                     AEArrayDataPointer arrayPtr; Size maximumSize;
                     DescType *itemType; Size *itemSize; long *itemCount);
OSErr AEPutArray    (AEDescList *theAEDescList; AEArrayType arrayType;
                     AEArrayDataPointer arrayPtr; DescType itemType;
                     Size itemSize; long itemCount);
OSErr AEDeleteItem  (AEDescList *theAEDescList; long index);
```

* `index == 0` appends; a positive index inserts/replaces at that position; `index == −1`
  replaces the last item (IM-IAC).
* `AECreateList`'s `factoringPtr`/`factoredSize` describe bytes common to every item of a
  *factored* list, as used by the array types (IM-IAC).
* The `get`/`size`/`delete` routines apply the same coercion rules as §2.3 and return
  `errAECoercionFail`, `errAEIllegalIndex`, `errAEBadListItem` as appropriate (IM-IAC).
* The `AERecord`-keyed spellings (`AEGetKeyPtr`, `AEGetKeyDesc`, `AESizeOfKeyDesc`,
  `AEDeleteKeyDesc`, `AEPutKeyPtr`, `AEPutKeyDesc`) are **not separate selectors**:
  Apple defines them as macros that call the corresponding `…Param…` routine
  (AE-HDR), and the pack likewise exposes only the `…Param…` / `…Nth…` selectors
  (PACK). A replacement needs no separate entry points.

### 2.3 Parameters and attributes of an AppleEvent

| selector | argBytes | routine |
|---|---:|---|
| `0x0E11` | 28 | `AEGetParamPtr(theAppleEvent, theAEKeyword, desiredType, typeCode, dataPtr, maximumSize, actualSize)` |
| `0x0812` | 16 | `AEGetParamDesc(theAppleEvent, theAEKeyword, desiredType, result)` |
| `0x0829` | 16 | `AESizeOfParam(theAppleEvent, theAEKeyword, typeCode, dataSize)` |
| `0x0A0F` | 20 | `AEPutParamPtr(theAppleEvent, theAEKeyword, typeCode, dataPtr, dataSize)` |
| `0x0610` | 12 | `AEPutParamDesc(theAppleEvent, theAEKeyword, theAEDesc)` |
| `0x0413` | 8 | `AEDeleteParam(theAppleEvent, theAEKeyword)` |
| `0x0E15` | 28 | `AEGetAttributePtr(theAppleEvent, theAEKeyword, desiredType, typeCode, dataPtr, maximumSize, actualSize)` |
| `0x0826` | 16 | `AEGetAttributeDesc(theAppleEvent, theAEKeyword, desiredType, result)` |
| `0x0828` | 16 | `AESizeOfAttribute(theAppleEvent, theAEKeyword, typeCode, dataSize)` |
| `0x0A16` | 20 | `AEPutAttributePtr(theAppleEvent, theAEKeyword, typeCode, dataPtr, dataSize)` |
| `0x0627` | 12 | `AEPutAttributeDesc(theAppleEvent, theAEKeyword, theAEDesc)` |

```
OSErr AEGetParamPtr (const AppleEvent *theAppleEvent; AEKeyword theAEKeyword;
                     DescType desiredType; DescType *typeCode; Ptr dataPtr;
                     Size maximumSize; Size *actualSize);
OSErr AEGetParamDesc(const AppleEvent *theAppleEvent; AEKeyword theAEKeyword;
                     DescType desiredType; AEDesc *result);
OSErr AESizeOfParam (const AppleEvent *theAppleEvent; AEKeyword theAEKeyword;
                     DescType *typeCode; Size *dataSize);
OSErr AEPutParamPtr (AppleEvent *theAppleEvent; AEKeyword theAEKeyword;
                     DescType typeCode; const void *dataPtr; Size dataSize);
OSErr AEPutParamDesc(AppleEvent *theAppleEvent; AEKeyword theAEKeyword;
                     const AEDesc *theAEDesc);
OSErr AEDeleteParam (AppleEvent *theAppleEvent; AEKeyword theAEKeyword);
```
(the four attribute routines are identical except that their first argument is the
event again and their keyword is looked up in the attribute part of the event).

* The `desiredType` parameter drives coercion; passing `typeWildCard` **suppresses**
  coercion, and `typeCode` then reports the original type (IM-IAC).
* If the data cannot be coerced, `errAECoercionFail` is returned (IM-IAC).
* `AEPutParamDesc` adds a *copy* of the descriptor and its data (IM-IAC).
* `AESizeOfParam` returns the size *after* coercion to `desiredType` (IM-IAC).
* If the keyword is absent the result is `errAEDescNotFound` (−1701) (IM-IAC).

### 2.4 Event dispatch table

| selector | argBytes | routine |
|---|---:|---|
| `0x091F` | 18 | `AEInstallEventHandler(theAEEventClass, theAEEventID, handler, handlerRefcon, isSysHandler)` |
| `0x0720` | 14 | `AERemoveEventHandler(theAEEventClass, theAEEventID, handler, isSysHandler)` |
| `0x0921` | 18 | `AEGetEventHandler(theAEEventClass, theAEEventID, handler, handlerRefcon, isSysHandler)` |
| `0x021B` | 4 | `AEProcessAppleEvent(theEventRecord)` |

```
OSErr AEInstallEventHandler(AEEventClass theAEEventClass; AEEventID theAEEventID;
                            EventHandlerProcPtr handler; long handlerRefcon;
                            Boolean isSysHandler);
OSErr AERemoveEventHandler (AEEventClass theAEEventClass; AEEventID theAEEventID;
                            EventHandlerProcPtr handler; Boolean isSysHandler);
OSErr AEGetEventHandler    (AEEventClass theAEEventClass; AEEventID theAEEventID;
                            EventHandlerProcPtr *handler; long *handlerRefcon;
                            Boolean isSysHandler);
OSErr AEProcessAppleEvent  (const EventRecord *theEventRecord);
```

Semantics (IM-IAC):

* `typeWildCard` (`'****'`) may be supplied for the class, the ID, or both; a wildcard
  matches anything. When both a specific and a wildcard entry match, the **more specific
  entry wins**.
* Installing over an existing `(class, id)` entry **replaces** it; the documented way to
  chain handlers is to fetch the old handler with `AEGetEventHandler` first.
* `isSysHandler = FALSE` selects the application table, `TRUE` the system table. A system
  handler must live in the system heap.
* `AERemoveEventHandler` removes the entry for the class/ID.

`AEProcessAppleEvent` is the dispatch algorithm, and it is a *requirement* (IM-IAC):

1. application **special** table, entry installed with `keyPreDispatch`;
2. application **event** table, most specific matching entry;
3. system special table, `keyPreDispatch`;
4. system event table, most specific matching entry;
5. otherwise return `errAEEventNotHandled` (−1708) to the server, and to the client if it
   is waiting for a reply.

Each step is skipped if no entry exists **or if the handler returned
`errAEEventNotHandled`**. A handler returning any *other* non-zero value is reporting a
genuine failure (IM-IAC). This is why handlers that do not understand an event must
return `errAEEventNotHandled`, and why the same value from a client handler is echoed
back to the client (IM-IAC).

The `EventRecord` passed in carries the event class in `message` and the event ID in
`where` (IM-IAC).

### 2.5 Coercion handler table

| selector | argBytes | routine |
|---|---:|---|
| `0x0A22` | 20 | `AEInstallCoercionHandler(fromType, toType, handler, handlerRefcon, fromTypeIsDesc, isSysHandler)` |
| `0x0723` | 14 | `AERemoveCoercionHandler(fromType, toType, handler, isSysHandler)` |
| `0x0B24` | 22 | `AEGetCoercionHandler(fromType, toType, handler, handlerRefcon, fromTypeIsDesc, isSysHandler)` |

```
OSErr AEInstallCoercionHandler(DescType fromType; DescType toType; ProcPtr handler;
                               long handlerRefcon; Boolean fromTypeIsDesc;
                               Boolean isSysHandler);
OSErr AERemoveCoercionHandler(DescType fromType; DescType toType; ProcPtr handler;
                              Boolean isSysHandler);
OSErr AEGetCoercionHandler(DescType fromType; DescType toType; ProcPtr *handler;
                           long *handlerRefcon; Boolean *fromTypeIsDesc;
                           Boolean isSysHandler);
```

* `fromTypeIsDesc` selects whether the handler is passed a descriptor record (`TRUE`) or
  a pointer to data (`FALSE`); Apple recommends the pointer form (IM-IAC).
* Installing over an existing `(fromType, toType)` entry replaces it (IM-IAC).
* The application table is consulted before the system table (IM-IAC).

### 2.6 Special handler table

| selector | argBytes | routine |
|---|---:|---|
| `0x0500` | 10 | `AEInstallSpecialHandler(functionClass, handler, isSysHandler)` |
| `0x0501` | 10 | `AERemoveSpecialHandler(functionClass, handler, isSysHandler)` |
| `0x052D` | 10 | `AEGetSpecialHandler(functionClass, handler, isSysHandler)` |

```
OSErr AEInstallSpecialHandler(AEKeyword functionClass; ProcPtr handler;
                              Boolean isSysHandler);
OSErr AERemoveSpecialHandler(AEKeyword functionClass; ProcPtr handler;
                             Boolean isSysHandler);
OSErr AEGetSpecialHandler(AEKeyword functionClass; ProcPtr *handler;
                          Boolean isSysHandler);
```

* Keyed by a single keyword. `keyPreDispatch` (`'phac'`) installs the handler that
  `AEProcessAppleEvent` runs before dispatch; the object-callback keywords of §5
  (`'xtok'`, `'cmpr'`, `'cont'`, `'mkid'`, `'mark'`, `'adjm'`, `'indc'`) are installed
  the same way (IM-IAC).
* Installing over an existing keyword replaces it (IM-IAC).
* `AEInstallSpecialHandler` can return `paramErr` (nil/odd handler), `memFullErr`
  (−108) and `errAENotASpecialFunction` (−1714, unknown keyword) (IM-IAC).

### 2.7 Creating, sending, current event, interaction

| selector | argBytes | routine |
|---|---:|---|
| `0x0B14` | 22 | `AECreateAppleEvent(theAEEventClass, theAEEventID, target, returnID, transactionID, result)` |
| `0x0D17` | 26 | `AESend(theAppleEvent, reply, sendMode, sendPriority, timeOutInTicks, idleProc, filterProc)` |
| `0x0441` | 8 | `AEManagerInfo(keyword, result)` |
| `0x0219` | 4 | `AEResetTimer(reply)` |
| `0x021A` | 4 | `AEGetTheCurrentEvent(theAppleEvent)` |
| `0x022C` | 4 | `AESetTheCurrentEvent(theAppleEvent)` |
| `0x022B` | 4 | `AESuspendTheCurrentEvent(theAppleEvent)` |
| `0x0818` | 16 | `AEResumeTheCurrentEvent(theAppleEvent, reply, dispatcher, handlerRefcon)` |
| `0x011E` | 2 | `AESetInteractionAllowed(level)` |
| `0x021D` | 4 | `AEGetInteractionAllowed(level)` |
| `0x061C` | 12 | `AEInteractWithUser(timeOutInTicks, nmReqPtr, idleProc)` |

```
OSErr AECreateAppleEvent(AEEventClass theAEEventClass; AEEventID theAEEventID;
                         const AEAddressDesc *target; Integer returnID;
                         long transactionID; AppleEvent *result);
OSErr AESend(AppleEvent *theAppleEvent; AppleEvent *reply; AESendMode sendMode;
             AESendPriority sendPriority; long timeOutInTicks;
             AEIdleUPP idleProc; AEFilterUPP filterProc);
OSErr AEManagerInfo(AEKeyword keyword; long *result);
OSErr AEResetTimer(const AppleEvent *reply);
OSErr AEGetTheCurrentEvent(AppleEvent *theAppleEvent);
OSErr AESetTheCurrentEvent(const AppleEvent *theAppleEvent);
OSErr AESuspendTheCurrentEvent(const AppleEvent *theAppleEvent);
OSErr AEResumeTheCurrentEvent(const AppleEvent *theAppleEvent,
                              const AppleEvent *reply,
                              EventHandlerProcPtr dispatcher, long handlerRefcon);
OSErr AESetInteractionAllowed(AEInteractAllowed level);
OSErr AEGetInteractionAllowed(AEInteractAllowed *level);
OSErr AEInteractWithUser(long timeOutInTicks; NMRecPtr nmReqPtr;
                         AEIdleUPP idleProc);
```

* `AECreateAppleEvent` builds an `'aevt'` record with the `keyAddressAttr`,
  `keyEventClassAttr`, `keyEventIDAttr` and `keyReturnIDAttr` attributes. `returnID` may
  be `kAutoGenerateReturnID` (−1); `transactionID` may be `kAnyTransactionID` (0)
  (IM-IAC).
* `AESend` (IM-IAC): the reply mode is one of `kAENoReply`, `kAEQueueReply`,
  `kAEWaitReply`; with `kAENoReply` the reply is a null descriptor; with `kAEQueueReply`
  the reply arrives in the event queue as class `'aevt'`, ID `'ansr'`; with
  `kAEWaitReply` the sender yields and should supply an idle procedure. A filter
  procedure may process events while waiting. `timeOutInTicks` may be
  `kAEDefaultTimeout` (−1) or `kNoTimeOut` (−2). `sendPriority` is
  `kAENormalPriority` (0) or `kAEHighPriority` (1).
* The caller owns the reply and must dispose of it with `AEDisposeDesc` (IM-IAC).
- `AEManagerInfo` (IM-IAC): `keyword` is `keyAEVersion` (`'vers'`) or
  `keyAERecorderCount` (`'recr'`). Version 1.0.1 and later. Its index (0x41) lies past
  the manager's dispatch table, so it must be handled outside the table (PACK).
* Interaction (IM-IAC): `level` is `kAEInteractWithSelf`, `kAEInteractWithLocal` or
  `kAEInteractWithAll`. `AEInteractWithUser` returns `noErr` if the application may now
  interact, `errAENoUserInteraction` (−1713) if it may not, and `errAETimeout` (−1712) on
  a timeout. It posts a notification request unless the client set `kAEWaitReply` and
  `kAECanSwitchLayer`, in which case the server may come straight to the front (IM-IAC).
* `AEResetTimer` extends the timeout of the reply event the current handler is answering
  (IM-IAC).
* `AESuspendTheCurrentEvent` / `AEResumeTheCurrentEvent` let a handler suspend its work
  (e.g. to await user input) and resume it later; `AEResumeTheCurrentEvent` may supply a
  substitute dispatcher (IM-IAC).

### 2.8 Required events

Applications written for System 7 are expected to install handlers for four events in
class `kCoreEventClass` (`'aevt'`) (IM-IAC, IM-VI):

| event ID | name | request |
|---|---|---|
| `'oapp'` | `kAEOpenApplication` | perform the normal open-application actions |
| `'odoc'` | `kAEOpenDocuments` | open the documents in `keyDirectObject` |
| `'pdoc'` | `kAEPrintDocuments` | print the documents in `keyDirectObject` |
| `'quit'` | `kAEQuitApplication` | quit |

If no handler at all is found for such an event, `AEProcessAppleEvent` returns
`errAEEventNotHandled` per §2.4 — the manager does **not** perform the action itself.

### 2.9 Object support — out-of-table selectors

These selectors are **absent from the manager's table** and reach the catch-all path of
§1. Their values are given because callers use them; their behaviour is the Object
Support Library's (IM-IAC chapter 6).

| selector | routine |
|---|---|
| `0x0536` | `AEResolve(objectSpecifier, callbackFlags, theToken)` |
| `0x023A` | `AEDisposeToken(theToken)` |
| `0x0C3B` | `AECallObjectAccessor(desiredClass, containerToken, containerClass, keyForm, keyData, token)` |
| `0x0937` | `AEInstallObjectAccessor(desiredClass, containerType, theAccessor, accessorRefcon, isSysHandler)` |
| `0x0738` | `AERemoveObjectAccessor(desiredClass, containerType, theAccessor, isSysHandler)` |
| `0x0939` | `AEGetObjectAccessor(desiredClass, containerType, theAccessor, accessorRefcon, isSysHandler)` |
| `0x0E35` | `AESetObjectCallbacks(myCompareProc, myCountProc, myDisposeTokenProc, myGetMarkTokenProc, myMarkProc, myAdjustMarksProc, myGetErrDescProc)` |

`AEObjectInit` is **not** a `Pack8` selector: Apple implements it by direct linkage, and
calling it "sets up the pack such that further calls can be via inline" (AE-HDR). A
replacement must provide it as a callable routine, not as a selector.

`AEResolve`'s `callbackFlags` are additive: `kAEIDoMinimum` (0), `kAEIDoWhose` (1),
`kAEIDoMarking` (4) — and, in later headers, `kAEPassSubDescs` (8),
`kAEResolveNestedLists` (16), `kAEHandleSimpleRanges` (32),
`kAEUseRelativeIterators` (64) (IM-IAC, AE-HDR). Object accessors are keyed by
`(desiredClass, containerType)`; `typeWildCard` may be used for either, and the more
specific entry wins (IM-IAC).

HyperCard depends on this layer: in one run it issued `0x0937` 52 times and `0x0E35`
once, and 23 `0x0A22` calls (HC).

### 2.10 Private handler-table selectors **_(undoc)_**

A second, undocumented family operates on the manager's handler-table primitive
directly. It is part of the interface: **HyperCard calls it**, allocates its own tables
with it, and never touches those tables except through these selectors (HC, PACK).

| selector | argBytes | routine |
|---|---:|---|
| `0x092E` | 18 | `_AE_hdlr_table_alloc` (undoc) |
| `0x042F` | 8 | `_AE_hdlr_table_free` (undoc) |
| `0x0830` | 16 | `_AE_hdlr_install_unique` (undoc) |
| `0x0831` | 16 | `_AE_hdlr_install` (undoc) |
| `0x0632` | 12 | `_AE_hdlr_delete` (undoc) |
| `0x0833` | 16 | `_AE_hdlr_lookup` (undoc) |
| `0x0834` | 16 | `_AE_hdlr_get_key` (undoc) |

No name in this table — nor any parameter name in the signatures below — is Apple's.
`_AE_hdlr_table_alloc`, `_AE_hdlr_install`, `_AE_hdlr_delete` and `_AE_hdlr_lookup`
are the names Executor's source uses (extra provenance note only); the others are
coined for this document.

```
OSErr _AE_hdlr_table_alloc(AE_hdlr_table_h *table, Byte systemZone,
                           const void *imports, long sizes, long capacityHint);
OSErr _AE_hdlr_table_free (const void *imports, AE_hdlr_table_h *table);
OSErr _AE_hdlr_install_unique(const AE_hdlr *hdlr, const AE_hdlr_selector *key,
                              const void *imports, AE_hdlr_table_h table);
OSErr _AE_hdlr_install      (const AE_hdlr *hdlr, const AE_hdlr_selector *key,
                             const void *imports, AE_hdlr_table_h table);
OSErr _AE_hdlr_delete       (const AE_hdlr_selector *key, const void *imports,
                             AE_hdlr_table_h table);
OSErr _AE_hdlr_lookup       (AE_hdlr *result, const AE_hdlr_selector *key,
                             const void *imports, AE_hdlr_table_h table);
OSErr _AE_hdlr_get_key      (AE_hdlr_selector *key, long index,
                             const void *imports, AE_hdlr_table_h table);
```

* `sizes` is packed `(valueSize << 16) | keySize`; the manager's own tables and every
  observed caller use `0x00080008` (PACK, HC). So an element is a key and a value, both
  8 bytes in practice.
* `systemZone` is a byte: non-zero allocates the table in the system zone (PACK).
* `capacityHint` is the **expected number of entries**, used to choose the initial size
  of the hash table; it is not a hard limit, and the size actually chosen is described in
  Appendix A. HyperCard passes 175, 105 and 16 for the three tables it creates; the
  manager's own zone tables use 8 (HC, PACK). *(An earlier reading of Executor's code
  treated this value as a hard capacity; (EXEC) is wrong here.)*
* `imports` is a pointer to a 4-entry vector of manager routines (`NewHandle`,
  `DisposeHandle`, `SetHandleSize`, deref-handle) or 0 to use the manager's own
  (PACK). Every observed caller passes 0.
* `_AE_hdlr_install` replaces; `_AE_hdlr_install_unique` refuses and reports a
  non-zero error if the key is already present (PACK). HyperCard uses the refusing
  form and treats any non-zero result as fatal to its setup (HC).
* `_AE_hdlr_delete` reports success even when the key is absent (PACK).
* `_AE_hdlr_lookup` copies the 8-byte value to `result`; it reports non-zero if the key
  is absent (PACK).
* `_AE_hdlr_table_free` disposes the table and stores NULL through `table` (PACK).
* `0x0834` is index-addressed rather than key-addressed and returns the *key* at that
  index; `0x042F`'s and `0x0834`'s exact purposes are inferred, and neither the manager
  nor HyperCard was observed calling them (PACK).

Because HyperCard never inspects its tables, the internal organisation of a handler
table is **not** part of this specification.

## 3. Types

```
AEDesc        = { DescType descriptorType; Handle dataHandle; }
AEDescList    = AEDesc
AERecord      = AEDescList
AppleEvent    = AERecord
AEAddressDesc = AEDesc
AEKeyDesc     = { AEKeyword descKey; AEDesc descContent; }

DescType   = ResType        /* a four-character code */
AEKeyword  = ResType
AEEventClass, AEEventID = ResType
AESendMode     = long  (flag bits, §5)
AESendPriority = Integer
AEEventSource  = Byte (0 unknown, 1 direct, 2 same process, 3 local, 4 remote)
AEInteractAllowed = Byte
AEArrayType    = Integer (0 data, 1 packed, 2 handle, 3 desc, 4 keydesc)
AEArrayData    = union of short[] / char[] / Handle[] / AEDesc[] / AEKeyDesc[]
AEArrayDataPointer = ^AEArrayData
```

An `AppleEvent` is an `AEDesc` of type `typeAppleEvent`; its data is a list of
keyword-specified (`AEKeyDesc`) entries, some of which are *attributes* rather than
parameters (IM-IAC).

Application-defined function types: `EventHandlerProcPtr`,
`IdleProcPtr`/`AEIdleUPP`, `EventFilterProcPtr`/`AEFilterUPP`, plus the coercion
procedures (`fromTypeIsDesc` selects the pointer or record form) and the Object Support
Library callbacks (`OSLCompareUPP`, `OSLCountUPP`, `OSLDisposeTokenUPP`,
`OSLGetMarkTokenUPP`, `OSLMarkUPP`, `OSLAdjustMarksUPP`, `OSLGetErrDescUPP`) (IM-IAC).

Undocumented types (PACK) — these are the arguments of §2.10 and the key/value pair of a
handler table:

```
AE_hdlr          = { void *fn; long refcon; }         /* 8 bytes */   (undoc)
AE_hdlr_selector = { long sel0;  long sel1; }         /* 8 bytes */   (undoc)
AE_hdlr_table_h  = handle to an opaque handler table                  (undoc)
```

For an event handler `sel0`/`sel1` are the event class and ID; for a coercion handler
the source and result descriptor types; for a special handler `sel1` is 0 (EXEC,
corroborated by PACK).

## 4. Error codes

Manager result codes (IM-IAC; values from MacErrors.h, AE-HDR):

| code | name |
|---:|---|
| 0 | `noErr` |
| −50 | `paramErr` |
| −108 | `memFullErr` |
| −1700 | `errAECoercionFail` |
| −1701 | `errAEDescNotFound` |
| −1702 | `errAECorruptData` |
| −1703 | `errAEWrongDataType` |
| −1704 | `errAENotAEDesc` |
| −1705 | `errAEBadListItem` |
| −1706 | `errAENewerVersion` |
| −1707 | `errAENotAppleEvent` |
| −1708 | `errAEEventNotHandled` |
| −1709 | `errAEReplyNotValid` |
| −1710 | `errAEUnknownSendMode` |
| −1711 | `errAEWaitCanceled` |
| −1712 | `errAETimeout` |
| −1713 | `errAENoUserInteraction` |
| −1714 | `errAENotASpecialFunction` |
| −1715 | `errAEParamMissed` |
| −1716 | `errAEUnknownAddressType` |
| −1717 | `errAEHandlerNotFound` |
| −1718 | `errAEReplyNotArrived` |
| −1719 | `errAEIllegalIndex` |
| −1720 | `errAEImpossibleRange` |
| −1721 | `errAEWrongNumberArgs` |
| −1722 | `errAEDuplicateKey` (undoc) — an entry for this key is already present (handler tables only) |
| −1723 | `errAEAccessorNotFound`; the handler-table selectors also return this number, with the internal meaning `errAEKeyNotFound` (undoc) |
| −1724 | `errAEIndexOutOfRange` (undoc) — index out of range (handler tables only) |
| −1725 | `errAENoSuchLogical` |
| −1726 | `errAEBadTestKey` |
| −1727 | `errAENotAnObjSpec` |
| −1728 | `errAENoSuchObject` |
| −1729 | `errAENegativeCount` |
| −1730 | `errAEEmptyListContainer` |
| −1731 | `errAEUnknownObjectType` |
| −1732 | `errAERecordingIsAlreadyOn` |
| −1733 | `errAEReceiveTerminate` |
| −1734 | `errAEReceiveEscapeCurrent` |
| −1735 | `errAEEventFiltered` |
| −1736 | `errAEDuplicateHandler` |
| −1737 | `errAEStreamBadNesting` |
| −1738 | `errAEStreamAlreadyConverted` |
| −1739 | `errAEDescIsNull` |
| −1740 | `errAEBuildSyntaxError` |
| −1741 | `errAEBufferTooSmall` |

Notable uses:

* `errAEEventNotHandled` is both the "no handler found" result of `AEProcessAppleEvent`
  and the value a handler returns to let the manager keep searching (§2.4).
* `errAEDescNotFound` for a missing keyword; `errAECoercionFail` when coercion fails;
  `errAEParamMissed` for a required parameter that was not supplied (IM-IAC).
* **Handler tables**: the three codes marked (undoc) above are the manager's *internal*
  codes — `errAEDuplicateKey` (−1722, from 0x0830), `errAEKeyNotFound` (−1723, from
  0x0833/0x0834) and `errAEIndexOutOfRange` (−1724, from 0x0834) (PACK). Note that
  −1723 is *also* the public `errAEAccessorNotFound`; the two are unrelated, and only
  the documented wrappers translate between the internal and the public layer (PACK).
  An implementation need only guarantee "zero on success, non-zero on failure" for the
  private selectors, since the observed external caller tests only for zero (HC).

## 5. Constants

**Selector** `selector = (argBytes/2) << 8 | index` (PACK).

**Descriptor types** (IM-IAC, AE-HDR) — the manager-level ones:

```
typeBoolean 'bool'   typeChar 'TEXT'      typeSInt16 'shor'   typeSInt32 'long'
typeUInt32  'magn'   typeIEEE32BitFloatingPoint 'sing'   typeIEEE64BitFloatingPoint 'doub'
typeAEList  'list'   typeAERecord 'reco'  typeAppleEvent 'aevt'
typeTrue    'true'   typeFalse 'fals'     typeType 'type'     typeEnumerated 'enum'
typeAlias   'alis'   typeFSS 'fss '       typeProcessSerialNumber 'psn '
typeWildCard '****'  typeNull  'null'     typeSectionH 'sect'
```
(Suite-specific types such as `typeObjectSpecifier` are in AERegistry; `typeWildCard`
is the wildcard for classes, IDs and descriptor types.)

**AppleEvent attribute keywords** (IM-IAC, AE-HDR):

```
keyAddressAttr 'addr'          keyEventClassAttr 'evcl'    keyEventIDAttr 'evid'
keyEventSourceAttr 'esrc'      keyInteractLevelAttr 'inte' keyMissedKeywordAttr 'miss'
keyOptionalKeywordAttr 'optk'  keyOriginalAddressAttr 'from' keyReturnIDAttr 'rtid'
keyTimeoutAttr 'timo'          keyTransactionIDAttr 'tran'
```

**Parameter keywords** (IM-IAC, AE-HDR):

```
keyDirectObject '----'   keyErrorNumber 'errn'   keyErrorString 'errs'
keyProcessSerialNumber 'psn '
```

**Event classes and IDs** (IM-IAC, AE-HDR):

```
kCoreEventClass 'aevt'   kAECoreSuite 'core'    kAEFinderEvents 'FNDR'
kSectionEventMsgClass 'sect'
kAEOpenApplication 'oapp'  kAEOpenDocuments 'odoc'
kAEPrintDocuments 'pdoc'   kAEQuitApplication 'quit'   kAEAnswer 'ansr'
```

**Send modes** (AESendMode, additive) (AE-HDR):

```
kAENoReply        0x00000001
kAEQueueReply     0x00000002
kAEWaitReply      0x00000003
kAENeverInteract  0x00000010
kAECanInteract    0x00000020
kAEAlwaysInteract 0x00000030
kAECanSwitchLayer 0x00000040
kAEDontReconnect  0x00000080
```

**Send priority, timeouts, return/transaction IDs** (AE-HDR):

```
kAENormalPriority 0     kAEHighPriority 1
kAEDefaultTimeout -1    kNoTimeOut -2
kAutoGenerateReturnID -1   kAnyTransactionID 0
```

**Interaction levels** (`AEInteractAllowed`) (AE-HDR):

```
kAEInteractWithSelf 0   kAEInteractWithLocal 1   kAEInteractWithAll 2
```

**Event sources** (`AEEventSource`) (AE-HDR):

```
kAEUnknownSource 0   kAEDirectCall 1   kAESameProcess 2
kAELocalProcess 3    kAERemoteProcess 4
```

**Array types** (`AEArrayType`) (AE-HDR):

```
kAEDataArray 0   kAEPackedArray 1   kAEHandleArray 2
kAEDescArray 3   kAEKeyDescArray 4
```

**`AEResolve` callback flags** (additive) (IM-IAC, AE-HDR):

```
kAEIDoMinimum 0x0000   kAEIDoWhose 0x0001   kAEIDoMarking 0x0004
kAEPassSubDescs 0x0008 kAEResolveNestedLists 0x0010
kAEHandleSimpleRanges 0x0020   kAEUseRelativeIterators 0x0040
```

**Special handler / object callback keywords** (IM-IAC, AE-HDR):

```
keyPreDispatch 'phac'   keySelectProc 'selh'
keyAECompareProc 'cmpr'    keyAECountProc 'cont'
keyDisposeTokenProc 'xtok' keyAEMarkTokenProc 'mkid'
keyAEMarkProc 'mark'       keyAEAdjustMarksProc 'adjm'
keyAEGetErrDescProc 'indc'
```
The pack's own keyword table also holds three keywords that have no public name. Named
here after their code letters only — so the names may not reflect their real meaning —
they are `keyAEBlock` `'blck'`, `keyAEUnblock` `'unbk'` and `keyAENavigate` `'navt'`
(undoc) (PACK).

**Object specifier key forms** (`keyForm`) (AE-HDR; values in AERegistry):

```
formAbsolutePosition 'indx'  formRelativePosition 'rele'  formTest 'test'
formRange 'rang'             formPropertyID 'prop'        formName 'name'
formUniqueID 'ID  '
```

**`AEManagerInfo` keywords** (IM-IAC, AE-HDR):

```
keyAEVersion 'vers'     keyAERecorderCount 'recr'
```

## Appendix A — the handler-table primitive (informational)

> **This appendix is not part of the specification.** Nothing outside the manager
> inspects a handler table: the observed external caller passes a table it obtained
> from `_AE_hdlr_table_alloc` (or from the zone record) straight back to the private
> selectors and never dereferences it (HC), and the pack exposes no table field of its
> own. An implementation may therefore use any representation at all (hash map, sorted
> array, linked list) and can safely ignore everything below. It is recorded only to
> document what the original PACK 8 does and to explain the two observable
> `_AE_hdlr_table_alloc` sizing parameters.

A table is a relocatable block; the handle's base (PACK):

| offset | size | meaning |
|---:|---:|---|
| 0x00 | long | hash mask (bin count − 1) |
| 0x04 | long | allocated size in bytes |
| 0x08 | long | element count |
| 0x0c | long | free-list head |
| 0x10 | long | element stride |
| 0x14 | long | per-table helper: `index → byte offset` (index × stride) |
| 0x18 | long | per-table helper: hash of a key |
| 0x1c | long | per-table helper (role unconfirmed) |
| 0x28 | word | log2(bin count) |
| 0x2a | word | growth threshold (0x50 for the manager's own tables) |
| 0x2e | word | `4 + keySize` |
| 0x30 | word | key size in bytes |
| 0x32 | word | value size in bytes |
| 0x34 | — | the elements |

Elements are fixed-stride records:

| offset | meaning |
|---:|---|
| 0x00 | occupancy / stored hash (0 = free) |
| 0x04 | key (`keySize` bytes) |
| 0x04+`keySize` | value (`valueSize` bytes) |

For the `keySize == valueSize == 8` case that every observed caller uses, the stride is
24.

**What `capacityHint` means.** It is the expected number of entries. The pack turns it
into an initial bucket count:

```
m       = 2^floor(log2(capacityHint - 1))   /* largest power of two strictly below */
buckets = max(8, 4 * m)                     /* capacityHint <= 1  =>  8 */
```

Because `(N-1)/2 < m <= N-1`, the initial load factor for N entries is between 0.25 and
0.5, and `buckets` is always a power of two, so it doubles as the index mask (`buckets-1`
is stored at +0x00 and `log2(buckets)` at +0x28). The handle is allocated as
`0x34 + (buckets + 6) * stride` bytes, providing `buckets + 6` element slots — the chains
plus headroom. Observed values: the manager's zone tables use 8 → 16 buckets;
HyperCard's tables use 16 → 32, 105 → 256 and 175 → 512 buckets (PACK, HC).

Hashing mixes the key's words through a rotate-5/add chain and a multiply by `0xb33d`,
forces bit 30 and toggles bit 31 to record the entry's chain state; the bucket index is
the result masked with the table's mask. Collisions are resolved by walking the bucket in
stride steps, with a bounded number of probes. `_AE_hdlr_table_alloc` does not compute
the stride or the helper addresses — it selects them from a small descriptor table
indexed by the packed `sizes`, which is why `sizes` and `capacityHint` are the only sizing
inputs; and it rehashes into a larger table once the element count passes the threshold
at +0x2a (PACK).

The exact size heuristic, the descriptor table's contents and the growth schedule were
not fully decoded, so treat the mechanics in this appendix as approximate. The field
offsets and layouts above were read directly (PACK).
