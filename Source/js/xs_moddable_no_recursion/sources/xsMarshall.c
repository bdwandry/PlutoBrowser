/*
 * Copyright (c) 2016-2025  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 * 
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 * 
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 * 
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and  
 * permission notice:  
 *
 *       Copyright (C) 2010-2016 Marvell International Ltd.
 *       Copyright (C) 2002-2010 Kinoma, Inc.
 *
 *       Licensed under the Apache License, Version 2.0 (the "License");
 *       you may not use this file except in compliance with the License.
 *       You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *       Unless required by applicable law or agreed to in writing, software
 *       distributed under the License is distributed on an "AS IS" BASIS,
 *       WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *       See the License for the specific language governing permissions and
 *       limitations under the License.
 */

#include "xsAll.h"

#if mx32bitID
#define mxSymbolBit 0x80000000
#define mxSymbolMask 0x7FFFFFFF
#else
#define mxSymbolBit 0x8000
#define mxSymbolMask 0x7FFF
#endif

typedef struct sxMarshallBuffer txMarshallBuffer; 
struct sxMarshallBuffer {
	txByte* base;
	txByte* current;
	txSlot* link;
	txID* symbolMap;
	txSize size;
	txID symbolCount;
	txSize symbolSize;
	txSlot* stack;
	c_jmp_buf jmp_buf;
	char error[128];
};

static void fxDemarshallChunk(txMachine* the, void* theData, void** theDataAddress);
static txID fxDemarshallKey(txMachine* the, txID id, txID* theSymbolMap, txBoolean alien);
static void fxDemarshallReference(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txID* theSymbolMap, txBoolean alien);
static void fxDemarshallSlot(txMachine* the, txSlot* theSlot, txSlot* theResult, txID* theSymbolMap, txBoolean alien);
static void fxMarshallChunk(txMachine* the, void* theData, void** theDataAddress, txMarshallBuffer* theBuffer);
static txBoolean fxMarshallKey(txMachine* the, txSlot* slot, txMarshallBuffer* theBuffer, txBoolean alien);
static void fxMarshallReference(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txMarshallBuffer* theBuffer, txBoolean alien);
static txBoolean fxMarshallSlot(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txMarshallBuffer* theBuffer, txBoolean alien);
static void fxMeasureChunk(txMachine* the, void* theData, txMarshallBuffer* theBuffer);
static txBoolean fxMeasureKey(txMachine* the, txID theID, txMarshallBuffer* theBuffer, txBoolean alien);
static void fxMeasureReference(txMachine* the, txSlot* theSlot, txMarshallBuffer* theBuffer, txBoolean alien);
static void fxMeasureSlot(txMachine* the, txSlot* theSlot, txMarshallBuffer* theBuffer, txBoolean alien);
static void fxMeasureThrow(txMachine* the, txMarshallBuffer* theBuffer, txString message);

#define mxMarshallAlign(POINTER,SIZE) \
	if (((SIZE) &= ((sizeof(txNumber) - 1)))) (POINTER) += sizeof(txNumber) - (SIZE)

/* xs_no_recursion (R11): the three structured-transfer walkers below
   (measure / marshall / demarshall) run on an explicit heap frame stack
   instead of the C stack. Their depth scales with the marshalled object
   graph and stock had only an mxCheckCStack guard that fires too late on a
   61.8KB game-task stack. No JavaScript runs inside the walk, so one
   static stack per process is safe; it is freed on every exit path
   (normal completion, fxMeasureThrow's longjmp, the mxCatch paths). */

typedef struct sxMarshallFrame txMarshallFrame;

struct sxMarshallFrame {
	txMarshallFrame* next;
	txInteger stage;
	txInteger route;
	txBoolean ok;
	txMarshallFrame* parent;
	txSlot* theSlot;
	txSlot* theResult;
	txSlot** theSlotAddress;
	txSlot* aSlot;
	txSlot** aSlotAddress;
	txSlot* aResult;
	txSlot* slot;
	txSlot* limit;
	txSlot elision;
	txInteger dense;
	txIndex index;
	txIndex offset;
};

static txMarshallFrame* gxMarshallFrames = C_NULL;

static void fxMarshallFramesFreeAll(void)
{
	while (gxMarshallFrames) {
		txMarshallFrame* frame = gxMarshallFrames;
		gxMarshallFrames = frame->next;
		c_free(frame);
	}
}

static txMarshallFrame* fxMarshallFramePush(txMachine* the)
{
	txMarshallFrame* frame = (txMarshallFrame*)c_malloc(sizeof(txMarshallFrame));
	if (!frame)
		fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
	c_memset(frame, 0, sizeof(txMarshallFrame));
	frame->next = gxMarshallFrames;
	gxMarshallFrames = frame;
	return frame;
}

static void fxMarshallFramePop(void)
{
	txMarshallFrame* frame = gxMarshallFrames;
	gxMarshallFrames = frame->next;
	c_free(frame);
}

/* demarshall stages and routes */
enum {
	mxDemarshallEnterStage = 0,
	mxDemarshallInstanceStage,
	mxDemarshallArrayStage,
	mxDemarshallListStage,
	mxDemarshallPrivateStage,
	mxDemarshallProxyHandlerStage,
	mxDemarshallProxyTargetStage
};

/* measure stages and routes */
enum {
	mxMeasureEnterStage = 0,
	mxMeasureInstanceStage,
	mxMeasureArrayStage,
	mxMeasureListStage,
	mxMeasurePrivateStage,
	mxMeasurePrivateChildrenStage,
	mxMeasureProxyHandlerStage,
	mxMeasureProxyTargetStage,
	mxMeasureDoneStage
};

enum {
	mxRouteMeasureNone = 0,
	mxRouteMeasureChild
};

static void fxMeasureSlotRun(txMachine* the, txMarshallBuffer* theBuffer, txBoolean alien);

static void fxMeasureChildDone(txMachine* the, txMarshallFrame* child)
{
	txMarshallFrame* parent = child->parent;
	if (!parent)
		return;
	/* stock pops the child's mxPushAt adornment right after its recursive
	   fxMeasureSlot returns; the adornment for the finished child is still
	   on the value stack here */
	switch (parent->stage) {
	case mxMeasureInstanceStage:
	case mxMeasureArrayStage:
	case mxMeasurePrivateChildrenStage:
		mxPop();
		break;
	case mxMeasureDoneStage:
		/* slow reference child: the parent pops itself when re-dispatched */
		break;
	}
}

static void fxMeasurePushChild(txMachine* the, txMarshallFrame* parent, txSlot* theSlot)
{
	txMarshallFrame* child = fxMarshallFramePush(the);
	child->stage = mxMeasureEnterStage;
	child->route = mxRouteMeasureChild;
	child->parent = parent;
	child->theSlot = theSlot;
}

enum {
	mxRouteDemarshallNone = 0,
	mxRouteDemarshallInstance,
	mxRouteDemarshallArray,
	mxRouteDemarshallList,
	mxRouteDemarshallPrivate,
	mxRouteDemarshallProxyHandler,
	mxRouteDemarshallProxyTarget
};

static void fxDemarshallSlotRun(txMachine* the, txID* theSymbolMap, txBoolean alien);

static void fxDemarshallChildDone(txMachine* the, txMarshallFrame* child)
{
	txMarshallFrame* parent = child->parent;
	if (!parent)
		return;
	switch (child->route) {
	case mxRouteDemarshallArray: {
		txSlot* aResult = parent->theResult->value.array.address + parent->offset;
		aResult->value = the->stack->value;
		aResult->kind = the->stack->kind;
		mxPop();
		*((txIndex*)aResult) = parent->index;
		parent->index++;
		parent->offset++;
		parent->theResult->value.array.length = parent->index;
		break;
	}
	case mxRouteDemarshallProxyHandler:
		parent->stage = mxDemarshallProxyTargetStage;
		break;
	case mxRouteDemarshallProxyTarget:
		parent->aSlot = C_NULL;
		break;
	}
}

static void fxDemarshallPushChild(txMachine* the, txMarshallFrame* parent, txInteger route, txSlot* theSlot, txSlot* theResult, txSlot** theSlotAddress)
{
	txMarshallFrame* child = fxMarshallFramePush(the);
	child->stage = mxDemarshallEnterStage;
	child->route = route;
	child->parent = parent;
	child->theSlot = theSlot;
	child->theResult = theResult;
	child->theSlotAddress = theSlotAddress;
}

/* every demarshall frame exit goes through here: the REFERENCE
   round-trip patch (stock runs its two trailing statements after the
   nested fxDemarshallSlot returns, whatever kind the child turned out
   to be), then advance the parent (ChildDone may consult the frame
   fields), then pop (stock pops after the callee returns) */
static void fxDemarshallFrameFinish(txMachine* the, txMarshallFrame* frame)
{
	txSlot* stashed = frame->aResult;
	if (stashed) {
		stashed->value.reference = stashed->value.error.info;
		stashed->kind = XS_REFERENCE_KIND;
	}
	fxDemarshallChildDone(the, frame);
	fxMarshallFramePop();
}

void fxDemarshall(txMachine* the, void* theData, txBoolean alien)
{
	txByte* p;
	txMarshallFrame* base;
	txByte* q;
	txID aSymbolCount;
	txID aSymbolLength;
	txID* aSymbolMap;
	txID* aSymbolPointer;
	txSlot* aSlot;
	txChunk* aChunk;
	txIndex aLength;
	
	if (!theData) {
		the->stack->kind = XS_UNDEFINED_KIND;
		return;
	}
	p = (txByte*)theData;
	q = p + *((txSize*)(p));
	base = gxMarshallFrames;
	{
		mxTry(the) {
			p += sizeof(txSize);
			aSymbolCount = *((txID*)p);
			p += sizeof(txID);
			aSymbolMap = aSymbolPointer = (txID*)p;
			p += aSymbolCount * sizeof(txID);
			while (aSymbolCount) {
				txID id;
				aSymbolLength = *aSymbolPointer;
				mxPushStringC((char *)p);
				id = fxNewName(the, the->stack);
				mxPop();
				*aSymbolPointer++ = id;
				aSymbolCount--;
				p += aSymbolLength;
			}
			aLength = mxPtrDiff(p - (txByte*)theData);
			mxMarshallAlign(p, aLength);
			mxPushUndefined();
			fxDemarshallSlot(the, (txSlot*)p, the->stack, aSymbolMap, alien);
		}
		mxCatch(the) {
			while (gxMarshallFrames != base)
				fxMarshallFramePop();
			the->stack->kind = XS_UNDEFINED_KIND;
			break;
		}
		while (p < q) {
			aSlot = (txSlot*)p;
			p += sizeof(txSlot);
			switch (aSlot->kind) {
			case XS_STRING_KIND:
			case XS_BIGINT_KIND:
				aChunk = (txChunk*)p;
				p += aChunk->size;
				mxMarshallAlign(p, aChunk->size);
				break;
			case XS_ARRAY_BUFFER_KIND:
				if (aSlot->value.arrayBuffer.address) {
					aChunk = (txChunk*)p;
					p += aChunk->size;
					mxMarshallAlign(p, aChunk->size);
				}
				break;
			case XS_REGEXP_KIND:
				if (aSlot->value.regexp.code) {
					aChunk = (txChunk*)p;
					p += aChunk->size;
					mxMarshallAlign(p, aChunk->size);
				}
				if (aSlot->value.regexp.data) {
					aChunk = (txChunk*)p;
					p += aChunk->size;
					mxMarshallAlign(p, aChunk->size);
				}
				break;
			case XS_KEY_KIND:
				if (aSlot->value.key.string) {
					aChunk = (txChunk*)p;
					p += aChunk->size;
					mxMarshallAlign(p, aChunk->size);
				}
				mxFallThrough;
			case XS_INSTANCE_KIND:
				aSlot->value.instance.garbage = C_NULL;
				break;
			}
		}
	}
}

void fxDemarshallChunk(txMachine* the, void* theData, void** theDataAddress)
{
	txSize aSize = ((txChunk*)(((txByte*)theData) - sizeof(txChunk)))->size - sizeof(txChunk);
	txByte* aResult = (txByte *)fxNewChunk(the, aSize);
	c_memcpy(aResult, theData, aSize);
	*theDataAddress = aResult;
}

txID fxDemarshallKey(txMachine* the, txID id, txID* theSymbolMap, txBoolean alien)
{
	if (id != XS_NO_ID) {
		if (alien)
			id = theSymbolMap[id - 1];
		else if (id >= the->keyOffset)
			id = theSymbolMap[id - the->keyOffset];
	}
	return id;
}

void fxDemarshallReference(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txID* theSymbolMap, txBoolean alien)
{
	if (!alien && (theSlot->flag & XS_DONT_MARSHALL_FLAG))
		*theSlotAddress = theSlot;
	else if (theSlot->value.instance.garbage)
		*theSlotAddress = theSlot->value.instance.garbage;
	else {
		/* stock: *theSlotAddress = fxNewSlot; fxDemarshallSlot(theSlot, *) —
		   the ENTER gate performs the fxNewSlot at the same point (nothing
		   runs between), so allocation order and the garbage bookkeeping are
		   identical */
		fxDemarshallPushChild(the, C_NULL, mxRouteDemarshallNone, theSlot, C_NULL, theSlotAddress);
		fxDemarshallSlotRun(the, theSymbolMap, alien);
	}
}

void fxDemarshallSlot(txMachine* the, txSlot* theSlot, txSlot* theResult, txID* theSymbolMap, txBoolean alien)
{
	/* xs_no_recursion (R11): iterative rewrite; the ENTER gate below
	   reproduces stock's switch statement for statement, in the same order,
	   so chunk copies, garbage bookkeeping, prototype sniffing and the
	   map/set rehash post-pass all happen exactly where stock does them. */
	fxDemarshallPushChild(the, C_NULL, mxRouteDemarshallNone, theSlot, theResult, C_NULL);
	fxDemarshallSlotRun(the, theSymbolMap, alien);
}

static void fxDemarshallSlotRun(txMachine* the, txID* theSymbolMap, txBoolean alien)
{
	while (gxMarshallFrames) {
		txMarshallFrame* frame = gxMarshallFrames;
		switch (frame->stage) {
		case mxDemarshallEnterStage: {
			txSlot* theSlot = frame->theSlot;
			txSlot* theResult;
			/* fxDemarshallReference route: stock allocated the result slot
			   just before calling fxDemarshallSlot; do it at the same point */
			if (frame->theSlotAddress && !frame->theResult) {
				*frame->theSlotAddress = fxNewSlot(the);
				frame->theResult = *frame->theSlotAddress;
			}
			theResult = frame->theResult;
			if (!(theSlot->flag & XS_INTERNAL_FLAG))
				theResult->ID = fxDemarshallKey(the, theSlot->ID, theSymbolMap, alien);
			else
				theResult->ID = theSlot->ID;
			theResult->flag = theSlot->flag;
			switch (theSlot->kind) {
			case XS_UNDEFINED_KIND:
			case XS_NULL_KIND:
			case XS_BOOLEAN_KIND:
			case XS_INTEGER_KIND:
			case XS_NUMBER_KIND:
			case XS_DATE_KIND:
			case XS_STRING_X_KIND:
			case XS_BIGINT_X_KIND:
			case XS_DATA_VIEW_KIND:
			case XS_KEY_X_KIND:
			case XS_BUFFER_INFO_KIND:
				theResult->value = theSlot->value;
				theResult->kind = theSlot->kind;
				break;
			case XS_STRING_KIND:
				fxDemarshallChunk(the, theSlot->value.string, (void **)&theResult->value.string);
				theResult->kind = theSlot->kind;
				break;
			case XS_BIGINT_KIND:
				fxDemarshallChunk(the, theSlot->value.bigint.data, (void **)&theResult->value.bigint.data);
				theResult->value.bigint.size = theSlot->value.bigint.size;
				theResult->value.bigint.sign = theSlot->value.bigint.sign;
				theResult->kind = theSlot->kind;
				break;
			case XS_ARRAY_BUFFER_KIND:
				if (theSlot->value.arrayBuffer.address)
					fxDemarshallChunk(the, theSlot->value.arrayBuffer.address, (void **)&(theResult->value.arrayBuffer.address));
				else
					theResult->value.arrayBuffer.address = C_NULL;
				theResult->kind = theSlot->kind;
				break;
			case XS_REGEXP_KIND:
				theResult->value.regexp.code = C_NULL;
				theResult->value.regexp.data = C_NULL;
				theResult->kind = theSlot->kind;
				if (theSlot->value.regexp.code)
					fxDemarshallChunk(the, theSlot->value.regexp.code, (void**)&(theResult->value.regexp.code));
				if (theSlot->value.regexp.data)
					fxDemarshallChunk(the, theSlot->value.regexp.data, (void**)&(theResult->value.regexp.data));
				break;
			case XS_KEY_KIND:
				if (theSlot->value.key.string)
					fxDemarshallChunk(the, theSlot->value.key.string, (void **)&theResult->value.key.string);
				else
					theResult->value.key.string = C_NULL;
				theResult->value.key.sum = theSlot->value.key.sum;
				theResult->kind = theSlot->kind;
				break;
			case XS_SYMBOL_KIND:
				theResult->value.symbol = fxDemarshallKey(the, theSlot->value.symbol, theSymbolMap, alien);
				theResult->kind = theSlot->kind;
				break;
			case XS_HOST_KIND:
				theResult->value.host.data = fxRetainSharedChunk(theSlot->value.host.data);
				theResult->value.host.variant.destructor = fxReleaseSharedChunk;
				theResult->kind = theSlot->kind;
				break;
			case XS_MAP_KIND:
			case XS_SET_KIND:
				theResult->value.table.length = theSlot->value.table.length;
				theResult->value.table.address = (txSlot**)fxNewChunk(the, theResult->value.table.length * sizeof(txSlot*));
				c_memset(theResult->value.table.address, 0, theResult->value.table.length * sizeof(txSlot*));
				theResult->kind = theSlot->kind;
				break;
			case XS_TYPED_ARRAY_KIND:
				theResult->value.typedArray.dispatch = (txTypeDispatch*)&gxTypeDispatches[theSlot->value.integer];
				theResult->value.typedArray.atomics = (txTypeAtomics*)&gxTypeAtomics[theSlot->value.integer];
				theResult->kind = theSlot->kind;
				break;
			case XS_REFERENCE_KIND: {
				/* stock: value.error.info = C_NULL; kind = ERROR;
				   fxDemarshallReference(child, &value.error.info);
				   value.reference = value.error.info; kind = REFERENCE */
				txSlot* childSlot = theSlot->value.reference;
				theResult->value.error.info = C_NULL;
				theResult->kind = XS_ERROR_KIND;
				if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG)) {
					theResult->value.error.info = childSlot;
				}
				else if (childSlot->value.instance.garbage) {
					theResult->value.error.info = childSlot->value.instance.garbage;
				}
				else {
					/* transform THIS frame into the child ENTER (its route/
					   parent already belong to us); on pop the round-trip
					   patch below runs (stock's two trailing statements) */
					*(&(theResult->value.error.info)) = fxNewSlot(the);
					frame->aResult = theResult;
					frame->theSlot = childSlot;
					frame->theResult = theResult->value.error.info;
					continue;
				}
				theResult->value.reference = theResult->value.error.info;
				theResult->kind = XS_REFERENCE_KIND;
				break;
			}
			case XS_INSTANCE_KIND: {
				txSlot* aSlot;
				theSlot->value.instance.garbage = theResult;
				theResult->value.instance.garbage = C_NULL;
				theResult->kind = theSlot->kind;
				aSlot = theSlot->next;
				if (!alien && theSlot->value.instance.prototype) {
					theResult->value.instance.prototype = theSlot->value.instance.prototype;
				}
				else {
					theResult->value.instance.prototype = mxObjectPrototype.value.reference;
					if (aSlot) {
						if (aSlot->flag & XS_INTERNAL_FLAG) {
							if (aSlot->ID == XS_ARRAY_BEHAVIOR)
								theResult->value.instance.prototype = mxArrayPrototype.value.reference;
							else {
								switch (aSlot->kind) {
								case XS_ARRAY_BUFFER_KIND: theResult->value.instance.prototype = mxArrayBufferPrototype.value.reference; break;
								case XS_BOOLEAN_KIND: theResult->value.instance.prototype = mxBooleanPrototype.value.reference; break;
								case XS_DATA_VIEW_KIND: theResult->value.instance.prototype = mxDataViewPrototype.value.reference; break;
								case XS_DATE_KIND: theResult->value.instance.prototype = mxDatePrototype.value.reference; break;
								case XS_ERROR_KIND: theResult->value.instance.prototype = mxErrorPrototypes(aSlot->value.error.which).value.reference; break;
								case XS_HOST_KIND: theResult->value.instance.prototype = mxSharedArrayBufferPrototype.value.reference; break;
								case XS_MAP_KIND: theResult->value.instance.prototype = mxMapPrototype.value.reference; break;
								case XS_NUMBER_KIND: theResult->value.instance.prototype = mxNumberPrototype.value.reference; break;
								case XS_REGEXP_KIND: theResult->value.instance.prototype = mxRegExpPrototype.value.reference; break;
								case XS_SET_KIND: theResult->value.instance.prototype = mxSetPrototype.value.reference; break;
								case XS_STRING_KIND: theResult->value.instance.prototype = mxStringPrototype.value.reference; break;
								case XS_TYPED_ARRAY_KIND: {
									txTypeDispatch* dispatch = (txTypeDispatch*)&gxTypeDispatches[aSlot->value.integer];
									mxPush(the->stackIntrinsics[-1 - (txInteger)dispatch->constructorID]);
									mxGetID(mxID(_prototype));
									theResult->value.instance.prototype = the->stack->value.reference;
									mxPop();
									} break;
								}
							}
						}
					}
				}
				frame->stage = mxDemarshallInstanceStage;
				frame->aSlot = aSlot;
				frame->aSlotAddress = &(theResult->next);
				frame->theResult = theResult;
				continue;
			}
			case XS_ARRAY_KIND:
				theResult->value.array.length = 0;
				theResult->value.array.address = C_NULL;
				theResult->kind = theSlot->kind;
				frame->theResult = theResult;
				frame->index = 0;
				frame->offset = 0;
				if ((frame->index = theSlot->value.array.length)) {
					frame->index = 0;
					theResult->value.array.address = (txSlot *)fxNewChunk(the, theSlot->value.array.length * sizeof(txSlot));
					c_memset(theResult->value.array.address, 0, theSlot->value.array.length * sizeof(txSlot));
					frame->stage = mxDemarshallArrayStage;
					frame->aSlot = theSlot->value.array.address;
					continue;
				}
				break;
			case XS_ERROR_KIND:
				theResult->value.error.info = C_NULL;
				theResult->value.error.which = theSlot->value.error.which;
				theResult->kind = theSlot->kind;
				if (theSlot->value.error.info) {
					txSlot* childSlot = theSlot->value.error.info;
					if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG)) {
						theResult->value.error.info = childSlot;
					}
					else if (childSlot->value.instance.garbage) {
						theResult->value.error.info = childSlot->value.instance.garbage;
					}
					else {
						/* stock: fxDemarshallReference(child, &info) slow path
						   = fxNewSlot + ENTER; no round-trip patch (aResult
						   stays NULL) */
						theResult->value.error.info = fxNewSlot(the);
						frame->theSlot = childSlot;
						frame->theResult = theResult->value.error.info;
						continue;
					}
				}
				break;
			case XS_PROXY_KIND:
				theResult->value.proxy.handler = C_NULL;
				theResult->value.proxy.target = C_NULL;
				theResult->kind = theSlot->kind;
				frame->theResult = theResult;
				frame->aSlot = theSlot->value.proxy.handler;
				frame->elision.value.at.id = theSlot->value.proxy.target ? 1 : 0;
				frame->elision.value.at.index = (txInteger)(uintptr_t)theSlot->value.proxy.target;
				frame->stage = mxDemarshallProxyHandlerStage;
				continue;
			case XS_LIST_KIND:
				theResult->value.list.first = C_NULL;
				theResult->value.list.last = C_NULL;
				theResult->kind = theSlot->kind;
				frame->stage = mxDemarshallListStage;
				frame->aSlot = theSlot->value.list.first;
				frame->aSlotAddress = &(theResult->value.list.first);
				frame->theResult = theResult;
				continue;
			case XS_PRIVATE_KIND:
				theResult->value.private.check = theSlot->value.private.check;
				theResult->value.private.first = C_NULL;
				theResult->kind = theSlot->kind;
				frame->stage = mxDemarshallPrivateStage;
				frame->aSlot = theSlot->value.private.first;
				frame->aSlotAddress = &(theResult->value.private.first);
				continue;
			default:
				break;
			}
			/* ENTER body finished */
			fxDemarshallFrameFinish(the, frame);
			continue;
		}
		case mxDemarshallInstanceStage:
			if (frame->aSlot) {
				txSlot* child = frame->aSlot;
				*frame->aSlotAddress = fxNewSlot(the);
				fxDemarshallPushChild(the, frame, mxRouteDemarshallInstance, child, *frame->aSlotAddress, C_NULL);
				frame->aSlot = child->next;
				frame->aSlotAddress = &((*frame->aSlotAddress)->next);
				continue;
			}
			{
			/* drained: stock's map/set rehash post-pass */
			txSlot* aSlot = frame->theResult->next;
			if (aSlot) {
				if (aSlot->kind == XS_MAP_KIND) {
					txSlot* key = aSlot->next->value.list.first;
					while (key) {
						txSlot* value = key->next;
						txU4 sum = fxSumEntry(the, key);
						txU4 modulo = sum % aSlot->value.table.length;
						txSlot* entry = fxNewSlot(the);
						txSlot** address = &(aSlot->value.table.address[modulo]);
						entry->next = *address;
						entry->kind = XS_ENTRY_KIND;
						entry->value.entry.slot = key;
						entry->value.entry.sum = sum;
						*address = entry;
						key = value->next;
					}
				}
				else if (aSlot->kind == XS_SET_KIND) {
					txSlot* key = aSlot->next->value.list.first;
					while (key) {
						txU4 sum = fxSumEntry(the, key);
						txU4 modulo = sum % aSlot->value.table.length;
						txSlot* entry = fxNewSlot(the);
						txSlot** address = &(aSlot->value.table.address[modulo]);
						entry->next = *address;
						entry->kind = XS_ENTRY_KIND;
						entry->value.entry.slot = key;
						entry->value.entry.sum = sum;
						*address = entry;
						key = key->next;
					}
				}		}
	}
		fxDemarshallFrameFinish(the, frame);
		continue;
		case mxDemarshallArrayStage:
			while (frame->aSlot) {
				txSlot* aSlot = frame->aSlot;
				if (aSlot->kind == XS_AT_KIND) {
					frame->index = aSlot->value.at.index;
					frame->aSlot = aSlot->next;
					continue;
				}
				mxPushUndefined();
				fxDemarshallPushChild(the, frame, mxRouteDemarshallArray, aSlot, the->stack, C_NULL);
				frame->aSlot = aSlot->next;
				goto wait_child;
			}
			fxDemarshallFrameFinish(the, frame);
			continue;
		case mxDemarshallListStage:
			if (frame->aSlot) {
				txSlot* child = frame->aSlot;
				frame->theResult->value.list.last = *frame->aSlotAddress = fxNewSlot(the);
				fxDemarshallPushChild(the, frame, mxRouteDemarshallList, child, *frame->aSlotAddress, C_NULL);
				frame->aSlot = child->next;
				frame->aSlotAddress = &((*frame->aSlotAddress)->next);
				continue;
			}
			fxDemarshallFrameFinish(the, frame);
			continue;
		case mxDemarshallPrivateStage:
			if (frame->aSlot) {
				txSlot* child = frame->aSlot;
				*frame->aSlotAddress = fxNewSlot(the);
				fxDemarshallPushChild(the, frame, mxRouteDemarshallPrivate, child, *frame->aSlotAddress, C_NULL);
				frame->aSlot = child->next;
				frame->aSlotAddress = &((*frame->aSlotAddress)->next);				continue;
			}
			fxDemarshallFrameFinish(the, frame);
			continue;

		case mxDemarshallProxyHandlerStage: {
			txSlot* childSlot = frame->aSlot;
			txSlot* theResult = frame->theResult;
			if (childSlot) {
				if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG)) {
					theResult->value.proxy.handler = childSlot;
					frame->stage = mxDemarshallProxyTargetStage;
				}
				else if (childSlot->value.instance.garbage) {
					theResult->value.proxy.handler = childSlot->value.instance.garbage;
					frame->stage = mxDemarshallProxyTargetStage;
				}
				else {
					theResult->value.proxy.handler = fxNewSlot(the);
					fxDemarshallPushChild(the, frame, mxRouteDemarshallProxyHandler, childSlot, theResult->value.proxy.handler, C_NULL);
				}
			}
			else
				frame->stage = mxDemarshallProxyTargetStage;
			continue;
		}
		case mxDemarshallProxyTargetStage: {
			txSlot* childSlot;
			txSlot* theResult = frame->theResult;
			if (frame->elision.value.at.id) {
				childSlot = (txSlot*)(uintptr_t)frame->elision.value.at.index;
				if (childSlot) {
					frame->elision.value.at.id = 0;
					if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG)) {
						theResult->value.proxy.target = childSlot;
						fxDemarshallFrameFinish(the, frame);
					}
					else if (childSlot->value.instance.garbage) {
						theResult->value.proxy.target = childSlot->value.instance.garbage;
						fxDemarshallFrameFinish(the, frame);
					}
					else {
						theResult->value.proxy.target = fxNewSlot(the);
						fxDemarshallPushChild(the, frame, mxRouteDemarshallProxyTarget, childSlot, theResult->value.proxy.target, C_NULL);
					}
					continue;
				}
			}
			fxDemarshallFrameFinish(the, frame);
			continue;
		}
		}
		wait_child:
		continue;
	}
}

void* fxMarshall(txMachine* the, txBoolean alien)
{
	txMarshallBuffer aBuffer;
	txSlot* aSlot;
	txSlot* bSlot;
	txSlot* cSlot;
	
	c_memset(&aBuffer, 0, sizeof(aBuffer));
	if (c_setjmp(aBuffer.jmp_buf) == 0) {
		size_t mapSize = alien ? the->keyIndex : (the->keyIndex - the->keyOffset);
		aBuffer.symbolSize = sizeof(txSize) + sizeof(txID);
		aBuffer.symbolMap = c_calloc(mapSize, sizeof(txID));
		if (mapSize && !aBuffer.symbolMap)
			fxMeasureThrow(the,  &aBuffer, "out of memory");
        the->stack->ID = XS_NO_ID;
        aBuffer.stack = the->stack;
		fxMeasureSlot(the, the->stack, &aBuffer, alien);
		
		aBuffer.size += aBuffer.symbolSize;
		mxMarshallAlign(aBuffer.size, aBuffer.symbolSize);
		aBuffer.base = aBuffer.current = (txByte *)c_malloc(aBuffer.size);
		if (!aBuffer.base)
			fxMeasureThrow(the,  &aBuffer, "out of memory");
		*((txSize*)(aBuffer.current)) = aBuffer.size;
		aBuffer.current += sizeof(txSize);
		*((txID*)(aBuffer.current)) = aBuffer.symbolCount;
		aBuffer.current += sizeof(txID);
		if (aBuffer.symbolCount) {
			txID* lengths = (txID*)aBuffer.current;
			txID* map = aBuffer.symbolMap;
			txID dstIndex = 1;
			txSlot** p;
			txSlot** q;
			txSlot* key;
			aBuffer.current += aBuffer.symbolCount * sizeof(txID);
			if (alien) {
				p = the->keyArrayHost;
				q = p + the->keyOffset;
				while (p < q) {
					txID length = *map;
					if (length) {
						*map = dstIndex;
						key = *p;
						mxCheck(the, (key->kind == XS_KEY_KIND) || (key->kind  == XS_KEY_X_KIND));
						*lengths++ = length;
						c_memcpy(aBuffer.current, key->value.key.string, length);
						aBuffer.current += length;
						dstIndex++;
					}
					map++;
					p++;
				}
			}
			else 
				dstIndex = the->keyOffset;
			p = the->keyArray;
			q = p + the->keyIndex - the->keyOffset;
			while (p < q) {
				txID length = *map;
				if (length) {
					*map = dstIndex;
					key = *p;
					mxCheck(the, (key->kind == XS_KEY_KIND) || (key->kind  == XS_KEY_X_KIND));
					*lengths++ = length;
					c_memcpy(aBuffer.current, key->value.key.string, length);
					aBuffer.current += length;
					dstIndex++;
				}
				map++;
				p++;
			}
		}
		mxMarshallAlign(aBuffer.current, aBuffer.symbolSize);
		
		fxMarshallSlot(the, the->stack, &aSlot, &aBuffer, alien);
		aSlot = aBuffer.link;
		while (aSlot) {
			bSlot = aSlot->value.instance.garbage;
			aSlot->flag &= ~XS_MARK_FLAG;
			aSlot->value.instance.garbage = C_NULL;
			aSlot = bSlot;
		}
		
		mxCheck(the, aBuffer.current == aBuffer.base + aBuffer.size);
	}
	else {
		fxMarshallFramesFreeAll();
		aSlot = the->firstHeap;
		while (aSlot) {
			bSlot = aSlot + 1;
			cSlot = aSlot->value.reference;
			while (bSlot < cSlot) {
				bSlot->flag &= ~XS_MARK_FLAG; 
				bSlot++;
			}
			aSlot = aSlot->next;
		}
		if (aBuffer.base)
			c_free(aBuffer.base);
		if (aBuffer.symbolMap)
			c_free(aBuffer.symbolMap);
		mxUnknownError(aBuffer.error);	
	}
	mxPop();
	c_free(aBuffer.symbolMap);
	return aBuffer.base;
}

void fxMarshallChunk(txMachine* the, void* theData, void** theDataAddress, txMarshallBuffer* theBuffer)
{
	txChunk* aChunk = ((txChunk*)(((txByte*)theData) - sizeof(txChunk)));
	txSize aSize = aChunk->size & 0x7FFFFFFF;
	txByte* aResult = theBuffer->current;
	theBuffer->current += aSize;
	c_memcpy(aResult, aChunk, aSize);
	aChunk = (txChunk*)aResult;
	aChunk->size &= 0x7FFFFFFF;
	*theDataAddress = aResult + sizeof(txChunk);
	mxMarshallAlign(theBuffer->current, aSize);
}

txBoolean fxMarshallKey(txMachine* the, txSlot* slot, txMarshallBuffer* theBuffer, txBoolean alien)
{
	txID id = slot->ID;
	txSlot* result = (txSlot*)(theBuffer->current);
	txSlot* key;
	if ((id == XS_NO_ID) || (slot->flag & XS_INTERNAL_FLAG)) {
		result->ID = id;
		return 1;
	}
	if (alien) {
		key = fxGetKey(the, id);
		if (key->flag & XS_DONT_ENUM_FLAG) {
			result->ID = theBuffer->symbolMap[id];
			return 1;
		}
	}
	else if (id >= the->keyOffset) {
		key = fxGetKey(the, id);
		if (key->flag & XS_DONT_ENUM_FLAG) {
			result->ID = theBuffer->symbolMap[id - the->keyOffset];
			return 1;
		}
	}
	else {
		result->ID = id;
		return 1;
	}
	return 0;
}

void fxMarshallReference(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txMarshallBuffer* theBuffer, txBoolean alien)
{
	if (!alien && (theSlot->flag & XS_DONT_MARSHALL_FLAG))
		*theSlotAddress = theSlot;
	else if (theSlot->value.instance.garbage)
		*theSlotAddress = theSlot->value.instance.garbage;
	else {
		/* xs_no_recursion (R11): stock's mxCheckCStack guard is gone — the
		   walkers below run on a heap frame stack, not the C stack */
		fxMarshallSlot(the, theSlot, theSlotAddress, theBuffer, alien);
	}
}

static void fxMarshallSlotRun(txMachine* the, txMarshallBuffer* theBuffer, txBoolean alien);

static txBoolean gxMarshallRootOK;

/* marshall stages and routes */
enum {
	mxMarshallEnterStage = 0,
	mxMarshallInstanceStage,
	mxMarshallArrayStage,
	mxMarshallListStage,
	mxMarshallPrivateStage,
	mxMarshallChildStage,
	mxMarshallProxyHandlerStage,
	mxMarshallProxyTargetStage,
	mxMarshallWaitChildStage,
	mxMarshallWaitProxyHandlerStage,
	mxMarshallWaitProxyTargetStage
};

enum {
	mxRouteMarshallNone = 0,
	mxRouteMarshallInstance,
	mxRouteMarshallArray,
	mxRouteMarshallList,
	mxRouteMarshallPrivate,
	mxRouteMarshallChild
};

/* xs_no_recursion (R11): called after the child frame has been popped;
   the child's route/ok verdict are passed in because the frame is gone */
static void fxMarshallChildDone(txMachine* the, txMarshallFrame* parent, txInteger route, txInteger ok, txMarshallBuffer* theBuffer, txBoolean alien)
{
	if (!parent)
		return;
	/* stock advances the output cursor per-child: array elements advance
	   unconditionally (they cannot be rejected), while instance
	   properties, list entries and private fields advance only when the
	   child's fxMarshallSlot returned 1 (it carved its result slot); a
	   rejected child's carve never happened, so the cursor stays and the
	   next child rewrites the same anchor, exactly like stock */
	switch (route) {
	case mxRouteMarshallArray:
		parent->aSlotAddress = &((*parent->aSlotAddress)->next);
		break;
	case mxRouteMarshallInstance:
		if (ok)
			parent->aSlotAddress = &((*parent->aSlotAddress)->next);
		break;
	case mxRouteMarshallList:
		parent->aResult->value.list.last = *parent->aSlotAddress;
		parent->aSlotAddress = &((*parent->aSlotAddress)->next);
		break;
	case mxRouteMarshallPrivate:
		parent->aSlotAddress = &((*parent->aSlotAddress)->next);
		break;
	}
	switch (parent->stage) {
	case mxMarshallInstanceStage:
	case mxMarshallListStage:
	case mxMarshallPrivateStage:
		parent->aSlot = parent->aSlot->next;
		break;
	case mxMarshallArrayStage:
		parent->slot++;
		break;
	case mxMarshallWaitChildStage:
		/* xs_no_recursion (R11): do NOT pop here; the wait frame must run
		   its own finish block so its route/ok cascade to ITS parent runs
		   (buffer cursor + source aSlot advance). The pump switch has no
		   case for wait stages, so the next dispatch falls through to the
		   finish block, which pops it. */
		break;
	case mxMarshallWaitProxyHandlerStage:
		parent->stage = mxMarshallProxyTargetStage;
		break;
	case mxMarshallWaitProxyTargetStage:
		/* xs_no_recursion (R11): same as WaitChildStage - let the wait
		   frame's own finish block complete it. */
		break;
	}
}

txBoolean fxMarshallSlot(txMachine* the, txSlot* theSlot, txSlot** theSlotAddress, txMarshallBuffer* theBuffer, txBoolean alien)
{
	/* xs_no_recursion (R11): iterative rewrite; the ENTER gate reproduces
	   stock's checks and switch in the same order, so buffer layout, the
	   carve-and-write of *theSlotAddress, mark/link bookkeeping, elision
	   slots and the output cursor behavior are identical to stock. */
	txMarshallFrame* frame = fxMarshallFramePush(the);
	frame->stage = mxMarshallEnterStage;
	frame->route = mxRouteMarshallNone;
	frame->theSlot = theSlot;
	frame->theSlotAddress = theSlotAddress;
	gxMarshallRootOK = 1;
	fxMarshallSlotRun(the, theBuffer, alien);
	return gxMarshallRootOK;
}

static void fxMarshallSlotRun(txMachine* the, txMarshallBuffer* theBuffer, txBoolean alien)
{
	while (gxMarshallFrames) {
		txMarshallFrame* frame = gxMarshallFrames;
		switch (frame->stage) {
		case mxMarshallEnterStage: {
			txSlot* theSlot = frame->theSlot;
			txSlot* aResult;
			frame->ok = 1;
			if ((theSlot->kind == XS_PRIVATE_KIND) && (alien || ~(theSlot->value.private.check->flag & XS_DONT_MARSHALL_FLAG))) {
				frame->ok = 0;
				break;
			}
			if (!fxMarshallKey(the, theSlot, theBuffer, alien)) {
				frame->ok = 0;
				break;
			}
			aResult = (txSlot*)(theBuffer->current);
			theBuffer->current += sizeof(txSlot);
			aResult->flag = theSlot->flag;
			aResult->kind = theSlot->kind;
			aResult->value = theSlot->value;
			*frame->theSlotAddress = aResult;
			frame->aResult = aResult;
			switch (theSlot->kind) {
			case XS_UNDEFINED_KIND:
			case XS_NULL_KIND:
			case XS_BOOLEAN_KIND:
			case XS_INTEGER_KIND:
			case XS_NUMBER_KIND:
			case XS_DATE_KIND:
			case XS_STRING_X_KIND:
			case XS_BIGINT_X_KIND:
			case XS_DATA_VIEW_KIND:
			case XS_KEY_X_KIND:
			case XS_BUFFER_INFO_KIND:
				break;
			case XS_STRING_KIND:
				fxMarshallChunk(the, theSlot->value.string, (void **)&(aResult->value.string), theBuffer);
				break;
			case XS_BIGINT_KIND:
				fxMarshallChunk(the, theSlot->value.bigint.data, (void **)&(aResult->value.bigint.data), theBuffer);
				break;
			case XS_ARRAY_BUFFER_KIND:
				if (theSlot->value.arrayBuffer.address)
					fxMarshallChunk(the, theSlot->value.arrayBuffer.address, (void **) &(aResult->value.arrayBuffer.address), theBuffer);
				break;
			case XS_REGEXP_KIND:
				if (theSlot->value.regexp.code)
					fxMarshallChunk(the, theSlot->value.regexp.code, (void**)&(aResult->value.regexp.code), theBuffer);
				if (theSlot->value.regexp.data)
					fxMarshallChunk(the, theSlot->value.regexp.data, (void**)&(aResult->value.regexp.data), theBuffer);
				break;
			case XS_KEY_KIND:
				if (theSlot->value.key.string)
					fxMarshallChunk(the, theSlot->value.key.string, (void **)&(aResult->value.key.string), theBuffer);
				break;
			case XS_SYMBOL_KIND:
				aResult->value.symbol = theSlot->value.symbol; //@@ only shared symbols remain
				break;
			case XS_HOST_KIND:
				break;
			case XS_MAP_KIND:
			case XS_SET_KIND:
				aResult->value.table.address = C_NULL;
				break;
			case XS_TYPED_ARRAY_KIND:
				aResult->value.integer = (txInteger)(theSlot->value.typedArray.dispatch - &gxTypeDispatches[0]);
				break;
			case XS_REFERENCE_KIND: {
				txSlot* childSlot = theSlot->value.reference;
				if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG)) {
					aResult->value.reference = childSlot;
				}
				else if (childSlot->value.instance.garbage) {
					aResult->value.reference = childSlot->value.instance.garbage;
				}
				else {
					/* stock: fxMarshallReference slow path = fxMarshallSlot
					   (child ENTER gate writes the anchor); no C-stack check
					   needed, the pump is heap-resident */
					frame->stage = mxMarshallWaitChildStage;
					frame->aSlot = childSlot;
					frame->aSlotAddress = &(aResult->value.reference);
					goto push_ref_child;
				}
				break;
			}
			case XS_INSTANCE_KIND: {
					aResult->value.instance.garbage = theBuffer->link;
				aResult->value.instance.prototype = C_NULL;
				{
				txSlot* protoSlot = theSlot->value.instance.prototype;
				if (!alien && protoSlot && (protoSlot->flag & XS_DONT_MARSHALL_FLAG))
					aResult->value.instance.prototype = protoSlot;
				}
				theSlot->value.instance.garbage = aResult;
				theBuffer->link = theSlot;
				frame->stage = mxMarshallInstanceStage;
				frame->aSlot = theSlot->next;
				frame->aSlotAddress = &(aResult->next);
				continue;
			}
			case XS_ARRAY_KIND: {
				txIndex length = theSlot->value.array.length;
				txIndex size = fxGetIndexSize(the, theSlot);
				aResult->value.array.length = size;
				frame->stage = mxMarshallArrayStage;
				frame->slot = theSlot->value.array.address;
				frame->limit = frame->slot + size;
				frame->aSlotAddress = &(aResult->value.array.address);
				frame->dense = (length == size);
				frame->elision = (txSlot){ NULL, {.ID = XS_NO_ID, .flag = XS_NO_FLAG, .kind = XS_AT_KIND}, .value = { .at = { 0x0, XS_NO_ID } } };
				continue;
			}
			case XS_ERROR_KIND:
				if (theSlot->value.error.info) {
					txSlot* childSlot = theSlot->value.error.info;
					if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG))
						aResult->value.error.info = childSlot;
					else if (childSlot->value.instance.garbage)
						aResult->value.error.info = childSlot->value.instance.garbage;
					else {
						frame->stage = mxMarshallWaitChildStage;
						frame->aSlot = childSlot;
						frame->aSlotAddress = &(aResult->value.error.info);
						goto push_ref_child;
					}
				}
				break;
			case XS_PROXY_KIND:
				if (theSlot->value.proxy.handler || theSlot->value.proxy.target) {
					frame->stage = mxMarshallProxyHandlerStage;
					frame->aSlot = theSlot->value.proxy.handler;
					frame->aSlotAddress = &(aResult->value.proxy.handler);
					frame->elision.value.at.id = theSlot->value.proxy.target ? 1 : 0;
					frame->elision.value.at.index = (txInteger)(uintptr_t)theSlot->value.proxy.target;					frame->aResult = aResult;
					continue;
				}
				break;
			case XS_LIST_KIND:
				frame->stage = mxMarshallListStage;
				frame->aSlot = theSlot->value.list.first;
				frame->aSlotAddress = &(aResult->value.list.first);
				frame->aResult = aResult;
				continue;
			case XS_PRIVATE_KIND:
				frame->stage = mxMarshallPrivateStage;
				frame->aSlot = theSlot->value.private.check;
				frame->aSlotAddress = &(aResult->value.private.first);
				frame->aResult = aResult;
				frame->theSlot = theSlot;
				continue;
			default:
				break;
			}
			break; /* ENTER body finished */
		}
		case mxMarshallInstanceStage:
		if (frame->aSlot) {
		/* stock: if (fxMarshallSlot(child, aSlotAddress)) advance the
		   output cursor — the advance is gated on the child carving its
		   slot, which ChildDone now sees via child->ok */
		txMarshallFrame* childFrame = fxMarshallFramePush(the);
				childFrame->stage = mxMarshallEnterStage;
				childFrame->route = mxRouteMarshallInstance;
				childFrame->parent = frame;
				childFrame->theSlot = frame->aSlot;
				childFrame->theSlotAddress = frame->aSlotAddress;
				continue;
			}
			*frame->aSlotAddress = C_NULL;
			break;
		case mxMarshallArrayStage:
			while (frame->slot < frame->limit) {
				txSlot* childSlot = frame->slot;
				if (!frame->dense) {
					txIndex idx = *((txIndex*)childSlot);
				if (frame->elision.value.at.index != idx) {
					txSlot* elisionResult;
					frame->elision.value.at.index = idx;
					/* stock: full fxMarshallSlot(&elision) call; its gate
					   always passes (XS_NO_ID) and the body writes one
					   AT-kind slot — inline the identical sequence */
					elisionResult = (txSlot*)(theBuffer->current);
					theBuffer->current += sizeof(txSlot);
					elisionResult->flag = frame->elision.flag;
					elisionResult->kind = frame->elision.kind;
					elisionResult->value = frame->elision.value;
					*frame->aSlotAddress = elisionResult;
					frame->aSlotAddress = &((*frame->aSlotAddress)->next);
				}
				frame->elision.value.at.index = idx + 1;
			}
			{
			txMarshallFrame* childFrame = fxMarshallFramePush(the);
			childFrame->stage = mxMarshallEnterStage;
			childFrame->route = mxRouteMarshallArray;
			childFrame->parent = frame;
			childFrame->theSlot = childSlot;
			childFrame->theSlotAddress = frame->aSlotAddress;
			}
			goto wait_child;
			}
			*frame->aSlotAddress = C_NULL;
			break;
		case mxMarshallListStage:
			if (frame->aSlot) {
				txSlot* child = frame->aSlot;
				txMarshallFrame* childFrame = fxMarshallFramePush(the);
				childFrame->stage = mxMarshallEnterStage;
				childFrame->route = mxRouteMarshallList;
				childFrame->parent = frame;
				childFrame->theSlot = child;
				childFrame->theSlotAddress = frame->aSlotAddress;
				continue;
			}
			*frame->aSlotAddress = C_NULL;
			break;
		case mxMarshallPrivateStage: {
			txSlot* check = frame->aSlot;
			if (!alien && check && (check->flag & XS_DONT_MARSHALL_FLAG)) {
				frame->aSlot = frame->theSlot->value.private.first;
				if (frame->aSlot) {
					txSlot* child = frame->aSlot;
					txMarshallFrame* childFrame = fxMarshallFramePush(the);
					childFrame->stage = mxMarshallEnterStage;
					childFrame->route = mxRouteMarshallPrivate;
					childFrame->parent = frame;
					childFrame->theSlot = child;
					childFrame->theSlotAddress = frame->aSlotAddress;
					continue;
				}
				*frame->aSlotAddress = C_NULL;
				break;
			}
			else {
				theBuffer->current -= sizeof(txSlot);
				frame->ok = 0;
				break;
			}
		}
		case mxMarshallChildStage:
			push_ref_child:
			{
			txMarshallFrame* childFrame = fxMarshallFramePush(the);
			childFrame->stage = mxMarshallEnterStage;
			childFrame->route = mxRouteMarshallChild;
			childFrame->parent = frame;
			childFrame->theSlot = frame->aSlot;
			childFrame->theSlotAddress = frame->aSlotAddress;
			}
			continue;
		case mxMarshallProxyHandlerStage: {
			txSlot* childSlot = frame->aSlot;
			if (childSlot) {
				/* stock: fxMarshallReference(handler, &aResult->...handler);
				   fast paths anchor directly, slow path recurses via
				   fxMarshallSlot (the child ENTER gate) */
				if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG))
					*frame->aSlotAddress = childSlot;
				else if (childSlot->value.instance.garbage)
					*frame->aSlotAddress = childSlot->value.instance.garbage;
				else {
					frame->stage = mxMarshallWaitProxyHandlerStage;
					goto push_ref_child;
				}
				if (frame->stage == mxMarshallProxyHandlerStage)
					frame->stage = mxMarshallProxyTargetStage;
				continue;
			}
			frame->stage = mxMarshallProxyTargetStage;
			continue;
		}
		case mxMarshallProxyTargetStage: {
			txSlot* childSlot = (txSlot*)(uintptr_t)frame->elision.value.at.index;
			if (frame->elision.value.at.id && childSlot) {
				frame->elision.value.at.id = 0;
				if (!alien && (childSlot->flag & XS_DONT_MARSHALL_FLAG))
					frame->aResult->value.proxy.target = childSlot;
				else if (childSlot->value.instance.garbage)
					frame->aResult->value.proxy.target = childSlot->value.instance.garbage;
				else {
					frame->stage = mxMarshallWaitProxyTargetStage;
					frame->aSlot = childSlot;
					frame->aSlotAddress = &(frame->aResult->value.proxy.target);
					goto push_ref_child;
				}
				continue;
			}
			break;
		}
		}
		/* frame body finished: record the root verdict, route to the
		   parent, then pop this frame (stock pops after the callee
		   returns — fxMarshallChildDone must see its fields) */
		{
		txInteger ok = frame->ok;
		txInteger route = frame->route;
		txMarshallFrame* parent = frame->parent;
		fxMarshallFramePop();
		/* ChildDone may pop a finished wait-stage parent, so it must run
		   with the child already off the stack (stock pops the callee's
		   C frame before running its trailing statements) */
		fxMarshallChildDone(the, parent, route, ok, theBuffer, alien);
		}
		continue;
		wait_child:
		continue;
	}
}

void fxMeasureChunk(txMachine* the, void* theData, txMarshallBuffer* theBuffer)
{
	txChunk* aChunk = ((txChunk*)(((txByte*)theData) - sizeof(txChunk)));
	txSize aSize = aChunk->size & 0x7FFFFFFF;
	theBuffer->size += aSize;
	if (theBuffer->size < 0)
		fxMeasureThrow(the, theBuffer, "too big");
	mxMarshallAlign(theBuffer->size, aSize);
}

txBoolean fxMeasureKey(txMachine* the, txID id, txMarshallBuffer* theBuffer, txBoolean alien)
{
	txSlot* key;
	txSize length;
	if (id == XS_NO_ID)
		return 1;
	if (alien) {
		if (theBuffer->symbolMap[id])
			return 1;
		key = fxGetKey(the, id);
		if (!(key->flag & XS_DONT_ENUM_FLAG))
			return 0;
	}
	else if (id >= the->keyOffset) {
		if (theBuffer->symbolMap[id - the->keyOffset])
			return 1;
		key = fxGetKey(the, id);
		if (!(key->flag & XS_DONT_ENUM_FLAG))
			return 0;
		id -= the->keyOffset;
	}
	else
		return 1;
	length = mxStringLength(key->value.key.string) + 1;	
	theBuffer->symbolMap[id] = (txID)length;
	theBuffer->symbolSize += sizeof(txID);
	theBuffer->symbolSize += length;
	theBuffer->symbolCount++;
	return 1;
}

/* xs_no_recursion (R11): fxMeasureReference/fxMeasureSlot are an iterative
   pump now. Stock recursed here guarded only by mxCheckCStack, which fires
   too late on a 61.8KB game-task stack; the walk below keeps its state in
   heap frames and its depth scales with the marshalled graph, not the C
   stack. The ENTER gate reproduces stock's fxMeasureSlot statement for
   statement (sizes, symbolMap population, MARK bits and fxMeasureThrow
   call sites are identical). */
void fxMeasureReference(txMachine* the, txSlot* theSlot, txMarshallBuffer* theBuffer, txBoolean alien)
{
	if (theSlot->flag & XS_DONT_MARSHALL_FLAG) {
		if (alien)
			if (theSlot->value.host.variant.destructor != fxReleaseSharedChunk)
				fxMeasureThrow(the, theBuffer, "read only object");
	}
	else if ((theSlot->flag & XS_MARK_FLAG) == 0)
		fxMeasureSlot(the, theSlot, theBuffer, alien);
}

void fxMeasureSlot(txMachine* the, txSlot* theSlot, txMarshallBuffer* theBuffer, txBoolean alien)
{
	/* xs_no_recursion (R11): iterative rewrite; the ENTER gate in the pump
	   reproduces stock's checks and switch in the same order, so sizes,
	   symbolMap population, MARK bits and throw sites are identical. */
	txMarshallFrame* frame = fxMarshallFramePush(the);
	frame->stage = mxMeasureEnterStage;
	frame->route = mxRouteMeasureNone;
	frame->theSlot = theSlot;
	fxMeasureSlotRun(the, theBuffer, alien);
}

static void fxMeasureSlotRun(txMachine* the, txMarshallBuffer* theBuffer, txBoolean alien)
{
	while (gxMarshallFrames) {
		txMarshallFrame* frame = gxMarshallFrames;
		switch (frame->stage) {
		case mxMeasureEnterStage: {
			txSlot* theSlot = frame->theSlot;
			if (!(theSlot->flag & XS_INTERNAL_FLAG) && !fxMeasureKey(the, theSlot->ID, theBuffer, alien)) {
				fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
				continue;
			}
			theBuffer->size += sizeof(txSlot);
			if (theBuffer->size < 0)
				fxMeasureThrow(the, theBuffer, "too big");
			switch (theSlot->kind) {
			case XS_UNDEFINED_KIND:
			case XS_NULL_KIND:
			case XS_BOOLEAN_KIND:
			case XS_INTEGER_KIND:
			case XS_NUMBER_KIND:
			case XS_DATE_KIND:
			case XS_STRING_X_KIND:
			case XS_BIGINT_X_KIND:
			case XS_DATA_VIEW_KIND:
			case XS_KEY_X_KIND:
			case XS_BUFFER_INFO_KIND:
				break;
				
			case XS_STRING_KIND:
				fxMeasureChunk(the, theSlot->value.string, theBuffer);
				break;
			case XS_BIGINT_KIND:
				fxMeasureChunk(the, theSlot->value.bigint.data, theBuffer);
				break;
			case XS_ARRAY_BUFFER_KIND: 
				if (theSlot->value.arrayBuffer.address)
					fxMeasureChunk(the, theSlot->value.arrayBuffer.address, theBuffer);
				break;
			case XS_REGEXP_KIND:
				if (theSlot->value.regexp.code)
					fxMeasureChunk(the, theSlot->value.regexp.code, theBuffer);
				if (theSlot->value.regexp.data)
					fxMeasureChunk(the, theSlot->value.regexp.data, theBuffer);
				break;	
			case XS_KEY_KIND:
				if (theSlot->value.key.string)
					fxMeasureChunk(the, theSlot->value.string, theBuffer);
				break;
				
			case XS_SYMBOL_KIND:
				if (!fxMeasureKey(the, theSlot->value.symbol, theBuffer, alien))
					fxMeasureThrow(the, theBuffer, "symbol");
				break;
				
			case XS_HOST_KIND: 
				if (theSlot->value.host.variant.destructor != fxReleaseSharedChunk)
					fxMeasureThrow(the, theBuffer, "host object");
				break;
			case XS_MAP_KIND:
			case XS_SET_KIND:
				break;	
			case XS_TYPED_ARRAY_KIND:
				break;
				
			case XS_REFERENCE_KIND: {
				/* stock: fxMeasureReference(child) inlined — its 3-way gate
				   decides whether the walk descends */
				txSlot* childSlot = theSlot->value.reference;
				if (childSlot->flag & XS_DONT_MARSHALL_FLAG) {
					if (alien)
						if (childSlot->value.host.variant.destructor != fxReleaseSharedChunk)
							fxMeasureThrow(the, theBuffer, "read only object");
				}
				else if ((childSlot->flag & XS_MARK_FLAG) == 0) {
					frame->stage = mxMeasureDoneStage;
					fxMeasurePushChild(the, frame, childSlot);
					continue;
				}
				break;
			}
			case XS_INSTANCE_KIND:
				theSlot->flag |= XS_MARK_FLAG;
				theSlot->value.instance.garbage = C_NULL;
				frame->stage = mxMeasureInstanceStage;
				frame->aSlot = theSlot->next;
				continue;
			case XS_ARRAY_KIND: {
				txIndex length = theSlot->value.array.length;
				txIndex size = fxGetIndexSize(the, theSlot);
				frame->stage = mxMeasureArrayStage;
				frame->slot = theSlot->value.array.address;
				frame->limit = frame->slot + size;
				frame->dense = (length == size);
				frame->elision = (txSlot){ NULL, {.ID = XS_NO_ID, .flag = XS_NO_FLAG, .kind = XS_AT_KIND}, .value = { .at = { 0x0, XS_NO_ID } } };
				continue;
			}
			case XS_ERROR_KIND:
				if (theSlot->value.error.info) {
					frame->stage = mxMeasureDoneStage;
					fxMeasurePushChild(the, frame, theSlot->value.error.info);
					continue;
				}
				break;
			case XS_PROXY_KIND:
				if (theSlot->value.proxy.handler || theSlot->value.proxy.target) {
					frame->stage = mxMeasureProxyHandlerStage;
					frame->aSlot = theSlot->value.proxy.handler;
					frame->elision.value.at.id = theSlot->value.proxy.target ? 1 : 0;
					frame->elision.value.at.index = (txInteger)(uintptr_t)theSlot->value.proxy.target;
					continue;
				}
				break;

			case XS_LIST_KIND:
				frame->stage = mxMeasureListStage;
				frame->aSlot = theSlot->value.list.first;
				continue;
			case XS_PRIVATE_KIND:
				frame->stage = mxMeasurePrivateStage;
				frame->aSlot = theSlot->value.private.check;
				continue;
				
			case XS_ARGUMENTS_SLOPPY_KIND:
			case XS_ARGUMENTS_STRICT_KIND:
				fxMeasureThrow(the, theBuffer, "arguments");
				break;
			case XS_CALLBACK_KIND:
			case XS_CALLBACK_X_KIND:
			case XS_CODE_KIND:
			case XS_CODE_X_KIND:
#if mxHostFunctionPrimitive
			case XS_HOST_FUNCTION_KIND:
#endif
				fxMeasureThrow(the, theBuffer, "function");
				break;
			case XS_FINALIZATION_CELL_KIND:
			case XS_FINALIZATION_REGISTRY_KIND:
				fxMeasureThrow(the, theBuffer, "finalization");
				break;
			case XS_MODULE_KIND:
			case XS_PROGRAM_KIND:
				fxMeasureThrow(the, theBuffer, "module");
				break;
			case XS_PROMISE_KIND:
				fxMeasureThrow(the, theBuffer, "promise");
				break;
			case XS_WEAK_MAP_KIND:
				fxMeasureThrow(the, theBuffer, "weak map");
				break;
			case XS_WEAK_REF_KIND:
				fxMeasureThrow(the, theBuffer, "weak ref");
				break;
			case XS_WEAK_SET_KIND:
				fxMeasureThrow(the, theBuffer, "weak set");
				break;
			case XS_ACCESSOR_KIND:
				fxMeasureThrow(the, theBuffer, "accessor");
				break;
			case XS_STACK_KIND:
				fxMeasureThrow(the, theBuffer, "generator");
				break;
			default:
				fxMeasureThrow(the, theBuffer, "no way");
				break;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		}
		case mxMeasureInstanceStage:
			if (frame->aSlot) {
				txSlot* childSlot = frame->aSlot;
				mxPushAt(childSlot->ID, 0);
				frame->aSlot = childSlot->next;
				fxMeasurePushChild(the, frame, childSlot);
				continue;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		case mxMeasureArrayStage:
			while (frame->slot < frame->limit) {
				txSlot* childSlot = frame->slot;
				if (!frame->dense) {
					txIndex index = *((txIndex*)childSlot);
					if (frame->elision.value.at.index != index) {
						frame->elision.value.at.index = index;
						theBuffer->size += sizeof(txSlot);
						if (theBuffer->size < 0)
							fxMeasureThrow(the, theBuffer, "too big");
					}
					frame->elision.value.at.index = index + 1;
				}
				mxPushAt(XS_NO_ID, *((txIndex*)childSlot));
				frame->slot++;
				fxMeasurePushChild(the, frame, childSlot);
				goto wait_child;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		case mxMeasureListStage:
			if (frame->aSlot) {
				txSlot* childSlot = frame->aSlot;
				frame->aSlot = childSlot->next;
				fxMeasurePushChild(the, frame, childSlot);
				continue;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		case mxMeasurePrivateStage: {
			txSlot* check = frame->aSlot;
			if (!alien && (check->flag & XS_DONT_MARSHALL_FLAG)) {
				frame->stage = mxMeasurePrivateChildrenStage;
				frame->aSlot = frame->theSlot->value.private.first;
				continue;
			}
			else
				theBuffer->size -= sizeof(txSlot);
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		}
		case mxMeasurePrivateChildrenStage:
			if (frame->aSlot) {
				txSlot* childSlot = frame->aSlot;
				mxPushAt(childSlot->ID, 0);
				frame->aSlot = childSlot->next;
				fxMeasurePushChild(the, frame, childSlot);
				continue;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		case mxMeasureDoneStage:
			/* the slow-reference parent pushed its only child and is done;
			   the child's completion lands here on re-dispatch */
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		case mxMeasureProxyHandlerStage: {
			txSlot* childSlot = frame->aSlot;
			if (childSlot) {
				/* stock: fxMeasureReference(handler) inlined */
				if (childSlot->flag & XS_DONT_MARSHALL_FLAG) {
					if (alien)
						if (childSlot->value.host.variant.destructor != fxReleaseSharedChunk)
							fxMeasureThrow(the, theBuffer, "read only object");
				}
				else if ((childSlot->flag & XS_MARK_FLAG) == 0) {
					/* stock returns from the handler subtree, then measures
					   the target — keep this frame alive for the target */
					frame->stage = mxMeasureProxyTargetStage;
					fxMeasurePushChild(the, frame, childSlot);
					continue;
				}
			}
			frame->stage = mxMeasureProxyTargetStage;
			continue;
		}
		case mxMeasureProxyTargetStage: {
			txSlot* childSlot = (txSlot*)(uintptr_t)frame->elision.value.at.index;
			if (frame->elision.value.at.id && childSlot) {
				frame->elision.value.at.id = 0;
				/* stock: fxMeasureReference(target) inlined */
				if (childSlot->flag & XS_DONT_MARSHALL_FLAG) {
					if (alien)
						if (childSlot->value.host.variant.destructor != fxReleaseSharedChunk)
							fxMeasureThrow(the, theBuffer, "read only object");
				}
				else if ((childSlot->flag & XS_MARK_FLAG) == 0)
					fxMeasurePushChild(the, frame, childSlot);
				continue;
			}
			fxMeasureChildDone(the, frame);
			fxMarshallFramePop();
			continue;
		}
		}
		wait_child:
		continue;
	}
}

void fxMeasureThrow(txMachine* the, txMarshallBuffer* theBuffer, txString message)
{
	/* xs_no_recursion (R11): the longjmp below skips the pumps' normal
	   unwinding — release the heap frame stack first (the error message
	   only reads theBuffer->stack adornments, which live on the value
	   stack, so freeing frames cannot disturb it) */
	fxMarshallFramesFreeAll();
	txSlot* slot = theBuffer->stack;
	txInteger i = 0;
	txInteger c = sizeof(theBuffer->error);
	i += c_snprintf(theBuffer->error, c, "marshall ");
	while (slot > the->stack) {
		slot--;
		if (slot->kind == XS_AT_KIND) {
			if (slot->value.at.id != XS_NO_ID) {
				txBoolean adorn;
				txString string = fxGetKeyString(the, slot->value.at.id, &adorn);
				if (adorn) {
					if (i < c) i += c_snprintf(theBuffer->error + i, c - i, "[%s]", string);
				}
				else {
					if (i < c) i += c_snprintf(theBuffer->error + i, c - i, ".%s", string);
				}
			}
			else {
				if (i < c) i += c_snprintf(theBuffer->error + i, c - i, "[%d]", (int)slot->value.at.index);
			}
		}
	}
	if (i < c)
		i += c_snprintf(theBuffer->error + i, c - i, ": %s", message);
	c_longjmp(theBuffer->jmp_buf, 1);
}

