/*
 * Copyright (c) 2016-2017  Moddable Tech, Inc.
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
#include "xsScript.h"

enum {
	XS_NO_JSON_TOKEN,
	XS_JSON_TOKEN_COLON,
	XS_JSON_TOKEN_COMMA,
	XS_JSON_TOKEN_EOF,
	XS_JSON_TOKEN_FALSE,
	XS_JSON_TOKEN_INTEGER,
	XS_JSON_TOKEN_LEFT_BRACE,
	XS_JSON_TOKEN_LEFT_BRACKET,
	XS_JSON_TOKEN_NULL,
	XS_JSON_TOKEN_NUMBER,
	XS_JSON_TOKEN_RIGHT_BRACE,
	XS_JSON_TOKEN_RIGHT_BRACKET,
	XS_JSON_TOKEN_STRING,
	XS_JSON_TOKEN_TRUE,
};

typedef struct {
	txSlot* slot;
	txSize offset;
	txInteger integer;
	txNumber number;
	txSlot* string;
	txInteger token;
	txSlot* keys;
	txInteger line;
	txBoolean sourceFlag;
	txInteger sourceOffset;
	txInteger sourceSize;
	/* xs_no_recursion: heap-resident frames for the iterative value/
	   object/array parser (fxJSONValueStep et al.); freed by c_free */
	void* walkStack;
	void* walkPool;
	int walkRunning;
} txJSONParser;

typedef struct {
	txString buffer;
	char indent[64];
	txInteger indentLength;
	txInteger level;
	txSize offset;
	txSize size;
	txSlot* replacer;
	txSlot* keys;
	txSlot* stack;
} txJSONStringifier;

static void fxParseJSON(txMachine* the, txJSONParser* theParser);
static void fxParseJSONArray(txMachine* the, txJSONParser* theParser);
static void fxParseJSONObject(txMachine* the, txJSONParser* theParser);
static void fxParseJSONToken(txMachine* the, txJSONParser* theParser);
static void fxParseJSONValue(txMachine* the, txJSONParser* theParser);
static void fxReviveJSON(txMachine* the, txJSONParser* theParser, txSlot* reviver);
static void fxJSONWalkFreeAllHook(txMachine* the);
static void fxJSONWalkFreeAll(txJSONParser* theParser);
static void fxReviveFreeAll(void);

static void fxStringifyJSON(txMachine* the, txJSONStringifier* theStringifier);
static void fxStringifyJSONCharacter(txMachine* the, txJSONStringifier* theStringifier, txInteger character);
static void fxStringifyJSONChars(txMachine* the, txJSONStringifier* theStringifier, char* s, txSize theSize);
static void fxStringifyJSONIndent(txMachine* the, txJSONStringifier* theStringifier);
static void fxStringifyJSONInteger(txMachine* the, txJSONStringifier* theStringifier, txInteger theInteger);
static void fxStringifyJSONName(txMachine* the, txJSONStringifier* theStringifier, txInteger* theFlag);
static void fxStringifyJSONNumber(txMachine* the, txJSONStringifier* theStringifier, txNumber theNumber);
static void fxStringifyJSONProperty(txMachine* the, txJSONStringifier* theStringifier, txInteger* theFlag);
static void fxStringifyJSONString(txMachine* the, txJSONStringifier* theStringifier, txString theString);
static void fxStringifyJSONUnicodeEscape(txMachine* the, txJSONStringifier* theStringifier, txInteger character);

static txSlot* fxToJSONKeys(txMachine* the, txSlot* reference);

void fxBuildJSON(txMachine* the)
{
	txSlot* slot;
	mxPush(mxObjectPrototype);
	slot = fxLastProperty(the, fxNewObjectInstance(the));
	slot = fxNextHostFunctionProperty(the, slot, mxCallback(fx_JSON_parse), 2, mxID(_parse), XS_DONT_ENUM_FLAG);
	slot = fxNextHostFunctionProperty(the, slot, mxCallback(fx_JSON_stringify), 3, mxID(_stringify), XS_DONT_ENUM_FLAG);
#if mxECMAScript2026
	slot = fxNextHostFunctionProperty(the, slot, mxCallback(fx_JSON_isRawJSON), 1, mxID(_isRawJSON), XS_DONT_ENUM_FLAG);
	slot = fxNextHostFunctionProperty(the, slot, mxCallback(fx_JSON_rawJSON), 1, mxID(_rawJSON), XS_DONT_ENUM_FLAG);
#endif
	slot = fxNextStringXProperty(the, slot, "JSON", mxID(_Symbol_toStringTag), XS_DONT_ENUM_FLAG | XS_DONT_SET_FLAG);
	mxPull(mxJSONObject);
}

#define mxIsRawJSON(THE_SLOT) \
	((THE_SLOT) && ((THE_SLOT)->next) && ((THE_SLOT)->next->flag & XS_INTERNAL_FLAG) && ((THE_SLOT)->next->kind == XS_RAW_JSON_KIND))

void fx_JSON_isRawJSON(txMachine* the)
{
	if (mxArgc < 1)
		mxTypeError("no text");
	txSlot* slot = mxArgv(0);
	mxResult->kind = XS_BOOLEAN_KIND;
	mxResult->value.boolean = (mxIsReference(slot) && mxIsRawJSON(slot->value.reference)) ? 1 : 0;
}

void fx_JSON_parse(txMachine* the)
{
	volatile txJSONParser aParser = {0};
	if (mxArgc < 1)
		mxSyntaxError("no buffer");
	fxToString(the, mxArgv(0));
	aParser.slot = mxArgv(0);
	aParser.offset = 0;
	mxPush(mxEmptyString);
	aParser.string = the->stack;
	aParser.line = 1;
	if ((mxArgc > 1) && mxIsReference(mxArgv(1))) {
		if (fxIsArray(the, mxArgv(1)->value.reference))
			aParser.keys = fxToJSONKeys(the, mxArgv(1));
		else if (mxIsCallable(mxArgv(1)->value.reference))
			aParser.sourceFlag = 1;
	}
	the->walkContext = (void*)&aParser;
	the->jsonWalkFree = fxJSONWalkFreeAllHook;
	/* xs_no_recursion (R5): the parse pump can throw (syntax error / OOM).
	   This catch is now the innermost jump target, so the interpreter-boundary
	   jsonWalkFree hook would NOT fire on the way out — free the abandoned
	   walk frames here, clear both machine fields (they must never outlive
	   this C frame, or a later unrelated throw fires them with a dangling
	   walkContext), then rethrow. */
	mxTry(the) {
		fxParseJSON(the, (txJSONParser*)&aParser);
	}
	mxCatch(the) {
		fxJSONWalkFreeAll(&aParser);
		the->jsonWalkFree = C_NULL;
		the->walkContext = C_NULL;
		fxJump(the);
	}
	the->jsonWalkFree = C_NULL;
	the->walkContext = C_NULL;
	if (aParser.sourceFlag) {
		txSlot* valueReference = the->stack + 1;
		txSlot* sourceReference = the->stack;
		txSlot* instance;
		txID id;
		mxPush(mxObjectPrototype);
		instance = fxNewObjectInstance(the);
		id = fxID(the, "");
		mxBehaviorDefineOwnProperty(the, instance, id, 0, valueReference, XS_GET_ONLY);
		mxPushSlot(mxArgv(1));
		mxCall();
		mxPushUndefined();
		fxKeyAt(the, id, 0, the->stack);
		mxPushSlot(valueReference);
		mxPushSlot(sourceReference);
		/* xs_no_recursion (R5): the reviver can throw (user code) or run out
		   of memory. A throw longjmps straight out of fxReviveJSON's pump;
		   free the abandoned revive frames (they are NOT covered by the
		   jsonWalkFree hook), clear both machine fields, rethrow. On the
		   success path clear the hooks too, so a stale walkContext can never
		   fire from a later unrelated throw inside some nested native. */
		mxTry(the) {
			fxReviveJSON(the, (txJSONParser*)&aParser, mxArgv(1));
			the->jsonWalkFree = C_NULL;
			the->walkContext = C_NULL;
		}
		mxCatch(the) {
			fxReviveFreeAll();
			the->jsonWalkFree = C_NULL;
			the->walkContext = C_NULL;
			fxJump(the);
		}
	}
	mxPullSlot(mxResult);
}

void fxParseJSON(txMachine* the, txJSONParser* theParser)
{
	fxParseJSONToken(the, theParser);
	fxParseJSONValue(the, theParser);
	if (theParser->token != XS_JSON_TOKEN_EOF)
		mxSyntaxError("%ld: missing EOF", theParser->line);
}

void fxParseJSONArray(txMachine* the, txJSONParser* theParser)
{
	txSlot* sourceArray = C_NULL;
	txSlot* sourceItem = C_NULL;
	txSlot* valueArray;
	txSlot* valueItem;
	txIndex length;

	mxCheckCStack();
	fxParseJSONToken(the, theParser);
	mxPush(mxArrayPrototype);
	valueArray = fxNewArrayInstance(the);
	valueItem = fxLastProperty(the, valueArray);
	if (theParser->sourceFlag) {
		mxPush(mxArrayPrototype);
		sourceArray = fxNewArrayInstance(the);
		sourceItem = fxLastProperty(the, sourceArray);
	}
	length = 0;
	for (;;) {
		if (theParser->token == XS_JSON_TOKEN_RIGHT_BRACKET)
			break;
		if (length) {
			if (theParser->token == XS_JSON_TOKEN_COMMA)
				fxParseJSONToken(the, theParser);
			else
				mxSyntaxError("%ld: missing ,", theParser->line);	
		}	
		fxParseJSONValue(the, theParser);
		length++;
		if (sourceItem) {
			sourceItem->next = fxNewSlot(the);
			sourceItem = sourceItem->next;
			sourceItem->kind = the->stack->kind;
			sourceItem->value = the->stack->value;
			mxPop();
		}
		valueItem->next = fxNewSlot(the);
		valueItem = valueItem->next;
		valueItem->kind = the->stack->kind;
		valueItem->value = the->stack->value;
		mxPop();
	}
	valueArray->next->value.array.length = length;
	fxCacheArray(the, valueArray);
	if (sourceItem) {
		sourceArray->next->value.array.length = length;
		fxCacheArray(the, sourceArray);
	}
	fxParseJSONToken(the, theParser);
}

void fxParseJSONToken(txMachine* the, txJSONParser* theParser)
{
	txInteger character;
	txBoolean escaped;
	txNumber number;
	txSize offset;
	txSize size;
	txString p, s;

	theParser->integer = 0;
	theParser->number = 0;
	theParser->string->value.string = mxEmptyString.value.string;
	theParser->string->kind = mxEmptyString.kind;
	theParser->token = XS_NO_JSON_TOKEN;
	p = theParser->slot->value.string + theParser->offset;
	while (theParser->token == XS_NO_JSON_TOKEN) {
		switch (*p) {
		case 0:
			theParser->token = XS_JSON_TOKEN_EOF;
			break;
		case 10:
			p++;
			theParser->line++;
			break;
		case 13:
			p++;
			theParser->line++;
			if (*p == 10)
				p++;
			break;
		case '\t':
		case ' ':
			p++;
			break;
		case '-':
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			s = p;
			if (*p == '-')
				p++;
			if (('0' <= *p) && (*p <= '9')) {
				if (*p == '0') {
					p++;
				}
				else {
					p++;
					while (('0' <= *p) && (*p <= '9'))
						p++;
				}
				if (*p == '.') {
					p++;
					if (('0' <= *p) && (*p <= '9')) {
						p++;
						while (('0' <= *p) && (*p <= '9'))
							p++;
					}
					else
						goto error;
				}
				if ((*p == 'e') || (*p == 'E')) {
					p++;
					if ((*p == '+') || (*p == '-'))
						p++;
					if (('0' <= *p) && (*p <= '9')) {
						p++;
						while (('0' <= *p) && (*p <= '9'))
							p++;
					}
					else
						goto error;
				}
			}
			else
				goto error;
			size = mxPtrDiff(p - s);
			if (theParser->sourceFlag) {
				theParser->sourceOffset = mxPtrDiff(s - theParser->slot->value.string);
				theParser->sourceSize = size;
			}
			if ((size_t)(size + 1) > sizeof(the->nameBuffer))
				mxSyntaxError("%ld: number overflow", theParser->line);
			c_memcpy(the->nameBuffer, s, size);
			the->nameBuffer[size] = 0;
			theParser->number = fxStringToNumber(the, the->nameBuffer, 0);
			theParser->integer = (txInteger)theParser->number;
			number = theParser->integer;
			if ((theParser->number == number) && (theParser->number != -0))
				theParser->token = XS_JSON_TOKEN_INTEGER;
			else
				theParser->token = XS_JSON_TOKEN_NUMBER;
			break;
		case ',':
			p++;
			theParser->token = XS_JSON_TOKEN_COMMA;
			break;	
		case ':':
			p++;
			theParser->token = XS_JSON_TOKEN_COLON;
			break;	
		case '[':
			p++;
			theParser->token = XS_JSON_TOKEN_LEFT_BRACKET;
			break;	
		case ']':
			p++;
			theParser->token = XS_JSON_TOKEN_RIGHT_BRACKET;
			break;	
		case '{':
			p++;
			theParser->token = XS_JSON_TOKEN_LEFT_BRACE;
			break;	
		case '}':
			p++;
			theParser->token = XS_JSON_TOKEN_RIGHT_BRACE;
			break;	
		case '"':
			s = p;
			p++;
			escaped = 0;
			offset = mxPtrDiff(p - theParser->slot->value.string);
			size = 0;
			for (;;) {
				p = mxStringByteDecode(p, &character);
				if (character < 32) {
					goto error;
				}
				else if (character == '"') {
					break;
				}
				else if (character == '\\') {
					escaped = 1;
					switch (*p) {
					case '"':
					case '/':
					case '\\':
					case 'b':
					case 'f':
					case 'n':
					case 'r':
					case 't':
						p++;
						size++;
						break;
					case 'u':
						p++;
						if (fxParseUnicodeEscape(&p, &character, 0, '\\'))
							size += mxStringByteLength(character);
						else
							goto error;
						break;
					default:
						goto error;
					}
				}
				else {
					size += mxStringByteLength(character);
				}
			}
			if (theParser->sourceFlag) {
				theParser->sourceOffset = mxPtrDiff(s - theParser->slot->value.string);
				theParser->sourceSize = mxPtrDiff(p - s);
			}
			s = theParser->string->value.string = fxNewChunk(the, size + 1);
			theParser->string->kind = XS_STRING_KIND;
			p = theParser->slot->value.string + offset;
			if (escaped) {
				for (;;) {
					if (*p == '"') {
						p++;
						*s = 0;
						break;
					}
					else if (*p == '\\') {
						p++;
						switch (*p) {
						case '"':
						case '/':
						case '\\':
							*s++ = *p++;
							break;
						case 'b':
							p++;
							*s++ = '\b';
							break;
						case 'f':
							p++;
							*s++ = '\f';
							break;
						case 'n':
							p++;
							*s++ = '\n';
							break;
						case 'r':
							p++;
							*s++ = '\r';
							break;
						case 't':
							p++;
							*s++ = '\t';
							break;
						case 'u':
							p++;
							fxParseUnicodeEscape(&p, &character, 0, '\\');
							s = mxStringByteEncode(s, character);
							break;
						}
					}
					else {
						*s++ = *p++;
					}
				}
			}
			else {
				c_memcpy(s, p, size);
				p += size + 1;
				s[size] = 0;
			}
			theParser->token = XS_JSON_TOKEN_STRING;
			break;
		case 'f':
			s = p;
			p++;
			if (*p != 'a') goto error;	
			p++;
			if (*p != 'l') goto error;	
			p++;
			if (*p != 's') goto error;	
			p++;
			if (*p != 'e') goto error;	
			p++;
			if (theParser->sourceFlag) {
				theParser->sourceOffset = mxPtrDiff(s - theParser->slot->value.string);
				theParser->sourceSize = mxPtrDiff(p - s);
			}
			theParser->token = XS_JSON_TOKEN_FALSE;
			break;
		case 'n':
			s = p;
			p++;
			if (*p != 'u') goto error;
			p++;
			if (*p != 'l') goto error;
			p++;
			if (*p != 'l') goto error;
			p++;
			if (theParser->sourceFlag) {
				theParser->sourceOffset = mxPtrDiff(s - theParser->slot->value.string);
				theParser->sourceSize = mxPtrDiff(p - s);
			}
			theParser->token = XS_JSON_TOKEN_NULL;
			break;
		case 't':
			s = p;
			p++;
			if (*p != 'r') goto error;
			p++;
			if (*p != 'u') goto error;
			p++;
			if (*p != 'e') goto error;
			p++;
			if (theParser->sourceFlag) {
				theParser->sourceOffset = mxPtrDiff(s - theParser->slot->value.string);
				theParser->sourceSize = mxPtrDiff(p - s);
			}
			theParser->token = XS_JSON_TOKEN_TRUE;
			break;
		default:
		error:
			mxSyntaxError("%ld: invalid character", theParser->line);	
			break;
		}
	}
	theParser->offset = mxPtrDiff(p - theParser->slot->value.string);
}

void fxParseJSONObject(txMachine* the, txJSONParser* theParser)
{
	txSlot* sourceObject = NULL;
	txSlot* valueObject;
	txBoolean comma = 0;
	txSlot* at;
	txIndex index;
	txID id;
	txSlot* property;

	mxCheckCStack();
	fxParseJSONToken(the, theParser);
	mxPush(mxObjectPrototype);
	valueObject = fxNewObjectInstance(the);
	if (theParser->sourceFlag) {
		mxPush(mxObjectPrototype);
		sourceObject = fxNewObjectInstance(the);
	}
	for (;;) {
		if (theParser->token == XS_JSON_TOKEN_RIGHT_BRACE)
			break;
		if (comma) {
			if (theParser->token == XS_JSON_TOKEN_COMMA)
				fxParseJSONToken(the, theParser);
			else
				mxSyntaxError("%ld: missing ,", theParser->line);	
		}	
		if (theParser->token != XS_JSON_TOKEN_STRING)
			mxSyntaxError("%ld: missing name", theParser->line);
		mxPushString(theParser->string->value.string);
		at = the->stack;
		index = 0;
		if (theParser->keys) {
			at->kind = XS_UNDEFINED_KIND;
			if (fxStringToIndex(the, at->value.string, &index))
				id = 0;
			else
				id = fxFindName(the, at->value.string);
			if (id != XS_NO_ID) {
				txSlot* item = theParser->keys->value.reference->next;
				while (item) {
					if ((item->value.at.id == id) && (item->value.at.index == index)) {
						at->value.at.id = id;
						at->value.at.index = index;
						at->kind = XS_AT_KIND;
						break;
					}
					item = item->next;
				}
			}
		}
		else {
			if (fxStringToIndex(the, at->value.string, &index))
				id = 0;
			else
				id = fxNewName(the, at);
			at->value.at.id = id;
            at->value.at.index = index;
			at->kind = XS_AT_KIND;
		}
		fxParseJSONToken(the, theParser);
		if (theParser->token != XS_JSON_TOKEN_COLON)
			mxSyntaxError("%ld: missing :", theParser->line);
		fxParseJSONToken(the, theParser);
		fxParseJSONValue(the, theParser);
		if (theParser->sourceFlag) {
			property = mxBehaviorSetProperty(the, sourceObject, at->value.at.id, at->value.at.index, XS_OWN);
			property->kind = the->stack->kind;
			property->value = the->stack->value;
			mxPop(); // source
		}
		if ((at->kind == XS_AT_KIND) && (the->stack->kind != XS_UNDEFINED_KIND)) {
			property = mxBehaviorSetProperty(the, valueObject, at->value.at.id, at->value.at.index, XS_OWN);
			property->kind = the->stack->kind;
			property->value = the->stack->value;
		}
		mxPop(); // value
		mxPop(); // at
		comma = 1;
	}
	fxParseJSONToken(the, theParser);
}

/* xs_no_recursion: iterative JSON parser. fxParseJSONValue/Object/Array
   recurse in stock; here each nested container pushes a heap frame
   (c_malloc'd, freed on pop and on the syntax-error longjmp) and the
   fxJSONValueStep pump drives the parse. Native C depth is constant. */

typedef struct sxJSONFrame sxJSONFrame;

struct sxJSONFrame {
	sxJSONFrame* next;
	int kind;		/* 1 = value, 2 = object, 3 = array */
	int stage;
	txSlot* valueContainer;
	txSlot* sourceContainer;
	txSlot* valueItem;
	txSlot* sourceItem;
	txSlot* at;
	txSlot* property;
	txIndex length;
	txIndex index;
	txID id;
	txBoolean comma;
};

#define kJSONValue 1
#define kJSONObject 2
#define kJSONArray 3

static void fxJSONWalkFreeAllHook(txMachine* the);

static void fxJSONWalkPush(txJSONParser* theParser, int kind)
{
	sxJSONFrame* frame = theParser->walkPool;
	if (frame)
		theParser->walkPool = frame->next;
	else
		frame = (sxJSONFrame*)c_malloc(sizeof(sxJSONFrame));
	if (!frame)
		fxAbort(C_NULL, XS_NOT_ENOUGH_MEMORY_EXIT);
	frame->next = (sxJSONFrame*)theParser->walkStack;
	frame->kind = kind;
	frame->stage = 0;
	frame->valueContainer = C_NULL;
	frame->sourceContainer = C_NULL;
	frame->valueItem = C_NULL;
	frame->sourceItem = C_NULL;
	frame->at = C_NULL;
	frame->property = C_NULL;
	frame->length = 0;
	frame->index = 0;
	frame->id = XS_NO_ID;
	frame->comma = 0;
	theParser->walkStack = frame;
}

static void fxJSONWalkPop(txMachine* the, txJSONParser* theParser)
{
	sxJSONFrame* frame = (sxJSONFrame*)theParser->walkStack;
	theParser->walkStack = frame->next;
	frame->next = (sxJSONFrame*)theParser->walkPool;
	theParser->walkPool = frame;
}

static void fxJSONWalkFreeAll(txJSONParser* theParser)
{
	sxJSONFrame* frame = (sxJSONFrame*)theParser->walkStack;
	while (frame) {
		sxJSONFrame* next = frame->next;
		frame->next = (sxJSONFrame*)theParser->walkPool;
		theParser->walkPool = frame;
		frame = next;
	}
	frame = (sxJSONFrame*)theParser->walkPool;
	while (frame) {
		sxJSONFrame* next = frame->next;
		c_free(frame);
		frame = next;
	}
	theParser->walkStack = C_NULL;
	theParser->walkPool = C_NULL;
}

static void fxJSONValueStep(txMachine* the, txJSONParser* theParser)
{
	sxJSONFrame* f = (sxJSONFrame*)theParser->walkStack;
	txSlot* value;
	txSlot* list;
	txSlot* slot;
	switch (f->kind) {
	case kJSONValue:
		switch (f->stage) {
		case 0:
			if (theParser->token == XS_JSON_TOKEN_LEFT_BRACE) {
				f->stage = 1;
				fxJSONWalkPush(theParser, kJSONObject);
				return;
			}
			if (theParser->token == XS_JSON_TOKEN_LEFT_BRACKET) {
				f->stage = 1;
				fxJSONWalkPush(theParser, kJSONArray);
				return;
			}
			switch (theParser->token) {
			case XS_JSON_TOKEN_FALSE:
				mxPushBoolean(0);
				break;
			case XS_JSON_TOKEN_TRUE:
				mxPushBoolean(1);
				break;
			case XS_JSON_TOKEN_NULL:
				mxPushNull();
				break;
			case XS_JSON_TOKEN_INTEGER:
				mxPushInteger(theParser->integer);
				break;
			case XS_JSON_TOKEN_NUMBER:
				mxPushNumber(theParser->number);
				break;
			case XS_JSON_TOKEN_STRING:
				mxPushString(theParser->string->value.string);
				break;
			default:
				mxPushUndefined();
				mxSyntaxError("%ld: invalid value", theParser->line);
				break;
			}
			if (theParser->sourceFlag) {
				value = the->stack;
				mxPushList();
				list = the->stack;
				slot = list->value.list.first = fxNewSlot(the);
				slot->kind = XS_DATA_VIEW_KIND;
				slot->value.dataView.offset = theParser->sourceOffset;
				slot->value.dataView.size = theParser->sourceSize;
				slot = slot->next = list->value.list.last = fxNewSlot(the);
				slot->kind = value->kind;
				slot->value = value->value;
			}
			fxParseJSONToken(the, theParser);
			fxJSONWalkPop(the, theParser);
			return;
		case 1:
			fxJSONWalkPop(the, theParser);
			return;
		}
		return;
	case kJSONObject: {
		txSlot* at;
		txIndex index;
		txID id;
		txSlot* property;
		switch (f->stage) {
		case 0:
			fxParseJSONToken(the, theParser);
			mxPush(mxObjectPrototype);
			f->valueContainer = fxNewObjectInstance(the);
			if (theParser->sourceFlag) {
				mxPush(mxObjectPrototype);
				f->sourceContainer = fxNewObjectInstance(the);
			}
			f->stage = 1;
			/* fall through */
		case 1:
			for (;;) {
				if (theParser->token == XS_JSON_TOKEN_RIGHT_BRACE)
					break;
				if (f->comma) {
					if (theParser->token == XS_JSON_TOKEN_COMMA)
						fxParseJSONToken(the, theParser);
					else
						mxSyntaxError("%ld: missing ,", theParser->line);	
				}
				if (theParser->token != XS_JSON_TOKEN_STRING)
					mxSyntaxError("%ld: missing name", theParser->line);
				mxPushString(theParser->string->value.string);
				f->at = the->stack;
				index = 0;
				if (theParser->keys) {
					f->at->kind = XS_UNDEFINED_KIND;
					if (fxStringToIndex(the, f->at->value.string, &index))
						id = 0;
					else
						id = fxFindName(the, f->at->value.string);
					if (id != XS_NO_ID) {
						txSlot* item = theParser->keys->value.reference->next;
						while (item) {
							if ((item->value.at.id == id) && (item->value.at.index == index)) {
								f->at->value.at.id = id;
								f->at->value.at.index = index;
								f->at->kind = XS_AT_KIND;
								break;
							}
							item = item->next;
						}
					}
				}
				else {
					if (fxStringToIndex(the, f->at->value.string, &index))
						id = 0;
					else
						id = fxNewName(the, f->at);
					f->at->value.at.id = id;
					f->at->value.at.index = index;
					f->at->kind = XS_AT_KIND;
				}
				fxParseJSONToken(the, theParser);
				if (theParser->token != XS_JSON_TOKEN_COLON)
					mxSyntaxError("%ld: missing :", theParser->line);
				fxParseJSONToken(the, theParser);
				f->stage = 2;
				fxJSONWalkPush(theParser, kJSONValue);
				return;
			}
			fxParseJSONToken(the, theParser);
			fxJSONWalkPop(the, theParser);
			return;
		case 2:
			if (theParser->sourceFlag) {
				property = mxBehaviorSetProperty(the, f->sourceContainer, f->at->value.at.id, f->at->value.at.index, XS_OWN);
				property->kind = the->stack->kind;
				property->value = the->stack->value;
				mxPop(); // source
			}
			if ((f->at->kind == XS_AT_KIND) && (the->stack->kind != XS_UNDEFINED_KIND)) {
				property = mxBehaviorSetProperty(the, f->valueContainer, f->at->value.at.id, f->at->value.at.index, XS_OWN);
				property->kind = the->stack->kind;
				property->value = the->stack->value;
			}
			mxPop(); // value
			mxPop(); // at
			f->comma = 1;
			f->stage = 1;
			return;
		}
		return;
	}
	case kJSONArray: {
		txSlot* valueArray;
		txSlot* valueItem;
		switch (f->stage) {
		case 0:
			fxParseJSONToken(the, theParser);
			mxPush(mxArrayPrototype);
			f->valueContainer = fxNewArrayInstance(the);
			f->valueItem = fxLastProperty(the, f->valueContainer);
			if (theParser->sourceFlag) {
				mxPush(mxArrayPrototype);
				f->sourceContainer = fxNewArrayInstance(the);
				f->sourceItem = fxLastProperty(the, f->sourceContainer);
			}
			f->length = 0;
			f->stage = 1;
			/* fall through */
		case 1:
			for (;;) {
				if (theParser->token == XS_JSON_TOKEN_RIGHT_BRACKET)
					break;
				if (f->length) {
					if (theParser->token == XS_JSON_TOKEN_COMMA)
						fxParseJSONToken(the, theParser);
					else
						mxSyntaxError("%ld: missing ,", theParser->line);	
				}
				f->stage = 2;
				fxJSONWalkPush(theParser, kJSONValue);
				return;
			}
			valueArray = f->valueContainer;
			valueItem = f->valueItem;
			valueArray->next->value.array.length = f->length;
			fxCacheArray(the, valueArray);
			if (f->sourceItem) {
				f->sourceContainer->next->value.array.length = f->length;
				fxCacheArray(the, f->sourceContainer);
			}
			fxParseJSONToken(the, theParser);
			fxJSONWalkPop(the, theParser);
			return;
		case 2:
			f->length++;
			if (f->sourceItem) {
				f->sourceItem->next = fxNewSlot(the);
				f->sourceItem = f->sourceItem->next;
				f->sourceItem->kind = the->stack->kind;
				f->sourceItem->value = the->stack->value;
				mxPop();
			}
			valueItem = f->valueItem;
			valueItem->next = fxNewSlot(the);
			f->valueItem = f->valueItem->next;
			f->valueItem->kind = the->stack->kind;
			f->valueItem->value = the->stack->value;
			mxPop();
			f->stage = 1;
			return;
		}
		return;
	}
	}
}

static void fxJSONWalkDrain(txMachine* the, txJSONParser* theParser);
static void fxJSONWalkFreeAllHook(txMachine* the);

static void fxJSONWalkDrain(txMachine* the, txJSONParser* theParser)
{
	while (theParser->walkStack)
		fxJSONValueStep(the, theParser);
}

/* xs_no_recursion: hook for the interpreter's catch path — frees any
   frames a syntax-error longjmp abandoned */
static void fxJSONWalkFreeAllHook(txMachine* the)
{
	if (the->walkContext)
		fxJSONWalkFreeAll((txJSONParser*)the->walkContext);
}

void fxParseJSONValue(txMachine* the, txJSONParser* theParser)
{
	/* xs_no_recursion: push a value frame and drain the pump */
	fxJSONWalkPush(theParser, kJSONValue);
	fxJSONWalkDrain(the, theParser);
}

#if 0	/* xs_no_recursion: original recursive bodies kept for reference */
static void fxParseJSONValue_ORIGINAL(txMachine* the, txJSONParser* theParser)
{
	if (theParser->token == XS_JSON_TOKEN_LEFT_BRACE)
		fxParseJSONObject(the, theParser);
	else if (theParser->token == XS_JSON_TOKEN_LEFT_BRACKET)
		fxParseJSONArray(the, theParser);
	else {
		switch (theParser->token) {
		case XS_JSON_TOKEN_FALSE:
			mxPushBoolean(0);
			break;
		case XS_JSON_TOKEN_TRUE:
			mxPushBoolean(1);
			break;
		case XS_JSON_TOKEN_NULL:
			mxPushNull();
			break;
		case XS_JSON_TOKEN_INTEGER:
			mxPushInteger(theParser->integer);
			break;
		case XS_JSON_TOKEN_NUMBER:
			mxPushNumber(theParser->number);
			break;
		case XS_JSON_TOKEN_STRING:
			mxPushString(theParser->string->value.string);
			break;
		default:
			mxPushUndefined();
			mxSyntaxError("%ld: invalid value", theParser->line);
			break;
		}
		if (theParser->sourceFlag) {
			txSlot* value = the->stack;
			txSlot* list;
			txSlot* slot;
			mxPushList();
			list = the->stack;
			slot = list->value.list.first = fxNewSlot(the);
			slot->kind = XS_DATA_VIEW_KIND;
			slot->value.dataView.offset = theParser->sourceOffset;
			slot->value.dataView.size = theParser->sourceSize;
			slot = slot->next = list->value.list.last = fxNewSlot(the);
			slot->kind = value->kind;
			slot->value = value->value;
		}
		fxParseJSONToken(the, theParser);
	}
}

#endif	/* xs_no_recursion: end original fxParseJSONValue for reference */

/* xs_no_recursion: iterative revive. The stock function recursed once
   per array element / object property (depth = container nesting).
   Here a frame per container holds the iteration cursor; nested values
   push a frame and the pump resumes the parent afterward. */
typedef struct sxJSONReviveFrame sxJSONReviveFrame;

struct sxJSONReviveFrame {
	sxJSONReviveFrame* next;
	txSlot* valueReference;
	txSlot* sourceReference;
	txSlot* reviver;
	txSlot* at;			/* object-key cursor (also length holder for arrays) */
	txIndex length;
	txIndex index;
};

static sxJSONReviveFrame* fxReviveStack;	/* revive runs outside the parser pump */

/* xs_no_recursion: free every frame a throw abandoned mid-revive. Called from
   fxReviveJSON's catch path (and safe to call when the stack is empty). */
static void fxReviveFreeAll(void)
{
	sxJSONReviveFrame* frame = fxReviveStack;
	while (frame) {
		sxJSONReviveFrame* next = frame->next;
		c_free(frame);
		frame = next;
	}
	fxReviveStack = C_NULL;
}

void fxReviveJSON(txMachine* the, txJSONParser* theParser, txSlot* reviver)
{
	sxJSONReviveFrame* frame = (sxJSONReviveFrame*)c_malloc(sizeof(sxJSONReviveFrame));
	if (!frame)
		fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
	c_memset(frame, 0, sizeof(sxJSONReviveFrame));
	frame->next = fxReviveStack;
	frame->valueReference = the->stack + 1;
	frame->sourceReference = the->stack;
	frame->reviver = reviver;
	fxReviveStack = frame;

	/* xs_no_recursion (R5): pump ONLY this invocation's frames, and stop the
	   moment the stack returns to the pre-invocation top. That base is
	   captured in a C local ONCE (base), never re-read from the heap: the
	   root frame is freed when it pops, so a condition like
	   "fxReviveStack != root->next" would be a use-after-free. The base is
	   non-NULL exactly when this is a RE-ENTRANT invocation (a reviver
	   called JSON.parse while an outer pump is blocked inside its
	   mxRunCount(3)); the outer's suspended frames below the base belong to
	   the outer pump — the inner must leave them untouched, otherwise it
	   consumes them and corrupts the outer's in-flight call window
	   (xs_no_recursion R5 re-entrancy bugfix). */
	sxJSONReviveFrame* base = frame->next;
	while (fxReviveStack != base) {
		txSlot* valueReference = frame->valueReference;
		txSlot* sourceReference = frame->sourceReference;
		int descend = 0;
		if (mxIsReference(valueReference)) {
			txSlot* instance = valueReference->value.reference;
			if (fxIsArray(the, instance)) {
				if (frame->length == 0) {
					mxPushSlot(valueReference);
					mxGetID(mxID(_length));
					frame->length = (txIndex)fxToLength(the, the->stack);
					mxPop();
				}
				while (frame->index < frame->length) {
					txIndex index = frame->index;
					mxPushSlot(valueReference);
					mxPushSlot(reviver);
					mxCall();
					mxPushUndefined();
					fxKeyAt(the, 0, index, the->stack);
					mxPushSlot(valueReference);
					mxGetIndex(index);
					if (mxIsReference(sourceReference)) {
						mxPushSlot(sourceReference);
						mxGetIndex(index);
					}
					else
						mxPushUndefined();
					frame->index++;
					descend = 1;
					break;
				}
			}
			else {
				if (!frame->at) {
					txSlot* at = fxNewInstance(the);
					mxBehaviorOwnKeys(the, instance, XS_EACH_NAME_FLAG, at);
					frame->at = at;
				}
				while ((frame->at = frame->at->next)) {
					txSlot* at = frame->at;
					mxPushSlot(valueReference);
					mxPushSlot(reviver);
					mxCall();
					mxPushUndefined();
					fxKeyAt(the, at->value.at.id, at->value.at.index, the->stack);
					mxPushSlot(valueReference);
					mxGetAll(at->value.at.id, at->value.at.index);
					if (mxIsReference(sourceReference)) {
						mxPushSlot(sourceReference);
						mxGetAll(at->value.at.id, at->value.at.index);
					}
					else
						mxPushUndefined();
					descend = 1;
					break;
				}
				if (!descend) {
					mxPop();	/* keys instance */
					frame->at = (txSlot*)1;	/* done marker */
				}
			}
			if (descend) {
				sxJSONReviveFrame* child = (sxJSONReviveFrame*)c_malloc(sizeof(sxJSONReviveFrame));
				if (!child)
					fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
				c_memset(child, 0, sizeof(sxJSONReviveFrame));
				child->next = fxReviveStack;
				child->valueReference = the->stack + 1;
				child->sourceReference = the->stack;
				child->reviver = reviver;
				fxReviveStack = child;
				frame = child;
				continue;
			}
		}
		/* container finished (or leaf): apply the source-wrapper check */
		if ((sourceReference->kind == XS_LIST_KIND) && fxIsSameValue(the, valueReference, sourceReference->value.list.last, 0)) {
			txSlot* view = sourceReference->value.list.first;
			txInteger offset = view->value.dataView.offset;
			txInteger size = view->value.dataView.size;
			txSlot* instance;
			txSlot* source;
			mxPop();
			mxPush(mxObjectPrototype);
			instance = fxNewObjectInstance(the);
			source = instance->next = fxNewSlot(the);
			source->value.string = fxNewChunk(the, size + 1);
			c_memcpy(source->value.string, theParser->slot->value.string + offset, size);
			source->value.string[size] = 0;
			source->kind = XS_STRING_KIND;
			source->ID = mxID(_source);
		}
		else {
			mxPop();
			mxPush(mxObjectPrototype);
			fxNewObjectInstance(the);
		}
		mxRunCount(3);
		/* deliver the revived value back to the parent frame. An inner
		   invocation never delivers below its root frame (the outer pump
		   handles the root's delivery when the inner returns). */
		sxJSONReviveFrame* parent = frame->next;
		fxReviveStack = parent;
		c_free(frame);
		frame = parent;
		if (parent && (parent != base)) {
			txSlot* valueReference = parent->valueReference;
			if (parent->reviver && mxIsReference(valueReference)) {
				txSlot* instance = valueReference->value.reference;
				if (fxIsArray(the, instance)) {
					txIndex index = parent->index - 1;
					if (mxIsUndefined(the->stack)) {
						mxBehaviorDeleteProperty(the, valueReference->value.reference, 0, index);
					}
					else {
						mxBehaviorDefineOwnProperty(the, valueReference->value.reference, 0, index, the->stack, XS_GET_ONLY);
					}
					mxPop();
				}
				else {
					/* object parent: at points AT the consumed key slot (the
				   object branch's while condition advanced to it before
				   pushing the child). Define/delete through it, then LEAVE
				   parent->at on the consumed key: the while condition
				   advances past it when the parent resumes. Advancing here
				   too double-stepped the cursor — after the last key it
				   nulled frame->at, which the object branch then mistook for
				   "keys not yet created" and re-parsed the object forever,
				   exhausting the value stack (xs_no_recursion R5 bugfix). */
				txSlot* at = parent->at;
				txInteger id = at->value.at.id;
				txInteger index = at->value.at.index;
				if (mxIsUndefined(the->stack)) {
					mxBehaviorDeleteProperty(the, valueReference->value.reference, id, index);
				}
				else {
					mxBehaviorDefineOwnProperty(the, valueReference->value.reference, id, index, the->stack, XS_GET_ONLY);
				}
				mxPop();
				}
			}
		}
	}
}

void fx_JSON_rawJSON(txMachine* the)
{
	txSlot* slot;
	txString string;
	txSize length;
	txSlot* instance;
	txSlot* property;
	volatile txJSONParser aParser = {0};
	if (mxArgc > 0)
		mxPushSlot(mxArgv(0));
	else
		mxPushUndefined();
	slot = the->stack;
	string = fxToString(the, slot);
	length = (txSize)c_strlen(string);
	if (length == 0) 
		mxSyntaxError("empty string");
	else {
		char first = string[0];
		char last = string[length - 1];
		if ((first == 0x09) || (first == 0x0A) || (first == 0x0D) || (first == 0x20) || (last == 0x09) || (last == 0x0A) || (last == 0x0D) || (last == 0x20))
		mxSyntaxError("invalid string");
	}
	aParser.slot = slot;
	aParser.offset = 0;
	mxPush(mxEmptyString);
	aParser.string = the->stack;
	aParser.line = 1;
	fxParseJSON(the, (txJSONParser*)&aParser);
	if (mxIsReference(the->stack))
		mxSyntaxError("invalid string");
	mxPop();
	instance = fxNewInstance(the);
	instance->flag |= XS_EXOTIC_FLAG | XS_DONT_PATCH_FLAG;
	property = instance->next = fxNewSlot(the);
	property->flag = XS_INTERNAL_FLAG | XS_DONT_DELETE_FLAG | XS_DONT_SET_FLAG;
	property->kind = XS_RAW_JSON_KIND;
	property = property->next = fxNewSlot(the);
	property->ID = mxID(_rawJSON);
	property->flag = XS_DONT_DELETE_FLAG | XS_DONT_SET_FLAG;
	property->kind = slot->kind;
	property->value = slot->value;
	mxPullSlot(mxResult);
}

void fx_JSON_stringify(txMachine* the)
{
	volatile txJSONStringifier aStringifier = {0};
	mxTry(the) {
		fxStringifyJSON(the, (txJSONStringifier*)&aStringifier);
		if (aStringifier.offset) {
			fxStringifyJSONChars(the, (txJSONStringifier*)&aStringifier, "\0", 1);
			mxResult->value.string = (txString)fxNewChunk(the, aStringifier.offset);
			c_memcpy(mxResult->value.string, aStringifier.buffer, aStringifier.offset);
			mxResult->kind = XS_STRING_KIND;
		}
		c_free(aStringifier.buffer);
	}
	mxCatch(the) {
		if (aStringifier.buffer)
			c_free(aStringifier.buffer);
		fxJump(the);
	}
}

void fxStringifyJSON(txMachine* the, txJSONStringifier* theStringifier)
{
	txSlot* aSlot;
	txInteger aFlag;
	txSlot* instance;
	
	aSlot = fxGetInstance(the, mxThis);
	theStringifier->offset = 0;
	theStringifier->size = 1024;
	theStringifier->buffer = c_malloc(1024);
	if (!theStringifier->buffer)
		fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);

	if (mxArgc > 1) {
		aSlot = mxArgv(1);
		if (mxIsReference(aSlot)) {
			if (fxIsCallable(the, aSlot))
				theStringifier->replacer = mxArgv(1);
			else if (fxIsArray(the, fxGetInstance(the, aSlot)))
				theStringifier->keys = fxToJSONKeys(the, aSlot);
		}
	}
	if (mxArgc > 2) {
		aSlot = mxArgv(2);
		if (mxIsReference(aSlot)) {
			txSlot* instance = fxGetInstance(the, aSlot);
			if (mxIsNumber(instance)) {
				fxToNumber(the, aSlot);
			}
			else if (mxIsString(instance)) {
				fxToString(the, aSlot);
			}
		}
		if ((aSlot->kind == XS_INTEGER_KIND) || (aSlot->kind == XS_NUMBER_KIND)) {
			txInteger aCount = fxToInteger(the, aSlot), anIndex;
			if (aCount < 0)
				aCount = 0;
			else if (aCount > 10)
				aCount = 10;
			for (anIndex = 0; anIndex < aCount; anIndex++)
				theStringifier->indent[anIndex] = ' ';
			theStringifier->indentLength = aCount;
		}
		else if (mxIsStringPrimitive(aSlot)) {
			txInteger aCount = fxUnicodeLength(aSlot->value.string, C_NULL);
			if (aCount > 10) {
				aCount = fxUnicodeToUTF8Offset(aSlot->value.string, 10);
			}
			else {
				aCount = (txInteger)c_strlen(aSlot->value.string);
			}
			c_memcpy(theStringifier->indent, aSlot->value.string, aCount);
			theStringifier->indent[aCount] = 0;
			theStringifier->indentLength = aCount;
		}
	}

	theStringifier->stack = the->stack;
	mxPush(mxObjectPrototype);
	instance = fxNewObjectInstance(the);
	aFlag = 0;
	if (mxArgc > 0)
		mxPushSlot(mxArgv(0));
	else
		mxPushUndefined();
	fxNextSlotProperty(the, instance, the->stack, mxID(__empty_string_), XS_NO_FLAG);
	mxPush(mxEmptyString);
	fxStringifyJSONProperty(the, theStringifier, &aFlag);
	mxPop();
}

void fxStringifyJSONCharacter(txMachine* the, txJSONStringifier* theStringifier, txInteger character)
{
    txSize size = mxStringByteLength(character);
	if ((theStringifier->offset + size) >= theStringifier->size) {
		char* aBuffer;
		theStringifier->size += ((size / 1024) + 1) * 1024;
		aBuffer = c_realloc(theStringifier->buffer, theStringifier->size);
		if (!aBuffer)
			fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
		theStringifier->buffer = aBuffer;
	}
	mxStringByteEncode(theStringifier->buffer + theStringifier->offset, character);
	theStringifier->offset += size;
}

void fxStringifyJSONChars(txMachine* the, txJSONStringifier* theStringifier, char* s, txSize theSize)
{
    //fprintf(stderr, "%s", s);
	if ((theStringifier->offset + theSize) >= theStringifier->size) {
		char* aBuffer;
		theStringifier->size += ((theSize / 1024) + 1) * 1024;
		aBuffer = c_realloc(theStringifier->buffer, theStringifier->size);
		if (!aBuffer)
			fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
		theStringifier->buffer = aBuffer;
	}
	c_memcpy(theStringifier->buffer + theStringifier->offset, s, theSize);
	theStringifier->offset += theSize;
}

void fxStringifyJSONIndent(txMachine* the, txJSONStringifier* theStringifier)
{
	txInteger aLevel;
	if (theStringifier->indent[0]) {
		fxStringifyJSONChars(the, theStringifier, "\n", 1);
		for (aLevel = 0; aLevel < theStringifier->level; aLevel++)
			fxStringifyJSONChars(the, theStringifier, theStringifier->indent, theStringifier->indentLength);
	}
}

void fxStringifyJSONInteger(txMachine* the, txJSONStringifier* theStringifier, txInteger theInteger)
{
	char aBuffer[256];
	fxIntegerToString(the, theInteger, aBuffer, sizeof(aBuffer));
	fxStringifyJSONChars(the, theStringifier, aBuffer, (txSize)c_strlen(aBuffer));
}

void fxStringifyJSONName(txMachine* the, txJSONStringifier* theStringifier, txInteger* theFlag)
{
	txSlot* aSlot = the->stack;
	if (*theFlag & 1) {
		fxStringifyJSONChars(the, theStringifier, ",", 1);
		fxStringifyJSONIndent(the, theStringifier);
	}
	else
		*theFlag |= 1;
	if (*theFlag & 2) {
		if (aSlot->kind == XS_INTEGER_KIND) {
			fxStringifyJSONChars(the, theStringifier, "\"", 1);
			fxStringifyJSONInteger(the, theStringifier, aSlot->value.integer);
			fxStringifyJSONChars(the, theStringifier, "\"", 1);
		}
		else
			fxStringifyJSONString(the, theStringifier, aSlot->value.string);
		fxStringifyJSONChars(the, theStringifier, ":", 1);
		if (theStringifier->indent[0])
			fxStringifyJSONChars(the, theStringifier, " ", 1);
	}
	mxPop(); // POP KEY
}

void fxStringifyJSONNumber(txMachine* the, txJSONStringifier* theStringifier, txNumber theNumber)
{
	int fpclass = c_fpclassify(theNumber);
	if ((fpclass != C_FP_NAN) && (fpclass != C_FP_INFINITE)) {
		char aBuffer[256];
		fxNumberToString(the, theNumber, aBuffer, sizeof(aBuffer), 0, 0);
		fxStringifyJSONChars(the, theStringifier, aBuffer, (txSize)c_strlen(aBuffer));
	}
	else
		fxStringifyJSONChars(the, theStringifier, "null", 4);
}

/* xs_no_recursion (R6): fxStringifyJSONProperty is iterative. Stock recursed
   once per array element / object property with only an mxCheckCStack guard
   (hard abort, too late on a 61.8KB game-task stack). Each stock invocation
   becomes a run on an explicit heap stack; the two recursive call sites push
   a child run that completes before the parent resumes at its saved cursor.
   The argument convention ([key][value][wrapper] pushed top-down) is
   preserved: a run stores the address of its key slot (stable across child
   pushes because child args are pushed ABOVE it) and derives value/wrapper
   pointers per phase, exactly like stock's aKey/aValue/aWrapper. The
   separator flag is the container's own cell (owned by the parent run):
   children receive a POINTER to it, so fxStringifyJSONName and the
   undefined branch mutate the same cell stock mutated (first child emits no
   comma, later children emit "," + indent; undefined elements set the flag
   for the next child). toJSON and replacer re-enter JavaScript exactly like
   stock (mxRunCount). The cycle guard (XS_LEVEL_FLAG set on container enter,
   cleared on normal completion AND on throw) matches stock's per-frame
   mxTry/mxCatch net state. Children push/pop their own args symmetrically,
   so parent anchors stay valid across child runs. */

typedef struct sxStringifyRun txStringifyRun;

struct sxStringifyRun {
	txStringifyRun* next;
	txInteger phase;
	txSlot* key;                /* address of this run's key slot */
	txSlot* instance;           /* container instance (ARRAY/OBJ phases) */
	txSlot* property;           /* scratch slot address (OBJ phase) */
	txSlot* at;                 /* keys cursor (OBJ phase) */
	txIndex index;              /* element cursor (ARRAY phase) */
	txIndex length;             /* element count (ARRAY phase) */
	txInteger ownFlag;          /* this container's separator cell */
	txInteger* flagAddress;     /* separator cell this run reads/mutates */
};

enum {
	mxStringifyHeadPhase = 0,
	mxStringifyArrayPhase,
	mxStringifyObjectPhase
};

static txStringifyRun* gxStringifyRuns = C_NULL;

static void fxStringifyRunsFreeAll(void)
{
	while (gxStringifyRuns) {
		txStringifyRun* run = gxStringifyRuns;
		gxStringifyRuns = run->next;
		c_free(run);
	}
}

static txStringifyRun* fxStringifyRunPush(txMachine* the)
{
	txStringifyRun* run = (txStringifyRun*)c_malloc(sizeof(txStringifyRun));
	if (!run)
		fxAbort(the, XS_NOT_ENOUGH_MEMORY_EXIT);
	c_memset(run, 0, sizeof(txStringifyRun));
	run->next = gxStringifyRuns;
	gxStringifyRuns = run;
	return run;
}

static void fxStringifyRunPop(void)
{
	txStringifyRun* run = gxStringifyRuns;
	gxStringifyRuns = run->next;
	c_free(run);
}

void fxStringifyJSONProperty(txMachine* the, txJSONStringifier* theStringifier, txInteger* theFlag)
{
	txStringifyRun* run = fxStringifyRunPush(the);
	run->phase = mxStringifyHeadPhase;
	run->key = the->stack;
	run->flagAddress = theFlag;
	while (gxStringifyRuns) {
		run = gxStringifyRuns;
		switch (run->phase) {

		case mxStringifyHeadPhase: {
			txSlot* aWrapper = run->key + 2;
			txSlot* aValue = run->key + 1;
			txSlot* aKey = run->key;
			txInteger* flagCell = run->flagAddress;
			txSlot* anInstance = C_NULL;
			mxCheckCStack();
			if (mxIsReference(aValue) || mxIsBigInt(aValue)) {
				/* THIS */
				mxPushSlot(aValue);
				/* FUNCTION */
				mxDub();
				mxGetID(mxID(_toJSON));
				if (mxIsReference(the->stack) && mxIsFunction(the->stack->value.reference)) {
					mxCall();
					mxPushSlot(aKey);
					fxToString(the, the->stack);
					mxRunCount(1);
					mxPullSlot(aValue);
				}
				the->stack = aKey;
			}
			if (theStringifier->replacer) {
				/* THIS */
				mxPushSlot(aWrapper);
				/* FUNCTION */
				mxPushSlot(theStringifier->replacer);
				mxCall();
				/* ARGUMENTS */
				mxPushSlot(aKey);
				fxToString(the, the->stack);
				mxPushSlot(aValue);
				/* COUNT */
				mxRunCount(2);
				mxPullSlot(aValue);
				the->stack = aKey;
			}
			if (mxIsReference(aValue)) {
				mxPushSlot(aValue);
				anInstance = fxToInstance(the, the->stack);
				if (anInstance->flag & XS_LEVEL_FLAG)
					mxTypeError("cyclic value");
				the->stack = aKey;
				{
					txSlot* aSlot = anInstance->next;
					if (aSlot && (aSlot->flag & XS_INTERNAL_FLAG)) {
						if ((aSlot->kind == XS_INTEGER_KIND) || (aSlot->kind == XS_NUMBER_KIND)) {
							fxToNumber(the, aValue);
						}
						else if (mxIsStringPrimitive(aSlot)) {
							fxToString(the, aValue);
						}
						else if ((aSlot->kind == XS_BOOLEAN_KIND) || (aSlot->kind == XS_BIGINT_KIND) || (aSlot->kind == XS_BIGINT_X_KIND)) {
							aValue->kind = aSlot->kind;
							aValue->value = aSlot->value;
						}
						else if (aSlot->kind == XS_RAW_JSON_KIND) {
							mxPushSlot(aValue);
							mxGetID(mxID(_rawJSON));
							aValue->kind = the->stack->kind;
							aValue->value = the->stack->value;
							the->stack = aKey;
							fxStringifyJSONName(the, theStringifier, flagCell);
							fxStringifyJSONChars(the, theStringifier, aValue->value.string, (txSize)c_strlen(aValue->value.string));
							mxPop(); // POP VALUE
							fxStringifyRunPop();
							continue;
						}
					}
				}
			}
			if (aValue->kind == XS_NULL_KIND) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				fxStringifyJSONChars(the, theStringifier, "null", 4);
			}
			else if (aValue->kind == XS_BOOLEAN_KIND) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				if (aValue->value.boolean)
					fxStringifyJSONChars(the, theStringifier, "true", 4);
				else
					fxStringifyJSONChars(the, theStringifier, "false", 5);
			}
			else if (aValue->kind == XS_INTEGER_KIND) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				fxStringifyJSONInteger(the, theStringifier, aValue->value.integer);
			}
			else if (aValue->kind == XS_NUMBER_KIND) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				fxStringifyJSONNumber(the, theStringifier, aValue->value.number);
			}
			else if ((aValue->kind == XS_STRING_KIND) || (aValue->kind == XS_STRING_X_KIND)) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				fxStringifyJSONString(the, theStringifier, aValue->value.string);
			}
			else if ((aValue->kind == XS_BIGINT_KIND) || (aValue->kind == XS_BIGINT_X_KIND)) {
				mxTypeError("stringify bigint");
			}
			else if ((aValue->kind == XS_REFERENCE_KIND) && !fxIsCallable(the, aValue)) {
				fxStringifyJSONName(the, theStringifier, flagCell);
				if (anInstance->flag & XS_MARK_FLAG)
					mxTypeError("read only value");
				anInstance->flag |= XS_LEVEL_FLAG;
				if (fxIsArray(the, anInstance)) {
					fxStringifyJSONChars(the, theStringifier, "[", 1);
					mxPushReference(anInstance);
					mxGetID(mxID(_length));
					run->length = fxToInteger(the, the->stack);
					run->instance = anInstance;
					run->index = 0;
					run->ownFlag = 4;
					if (run->length > 0) {
						theStringifier->level++;
						fxStringifyJSONIndent(the, theStringifier);
						mxPop();
						run->phase = mxStringifyArrayPhase;
						continue;
					}
					fxStringifyJSONChars(the, theStringifier, "]", 1);
					anInstance->flag &= ~XS_LEVEL_FLAG;
				}
				else {
					txSlot* at;
					fxStringifyJSONChars(the, theStringifier, "{", 1);
					if (theStringifier->keys) {
						mxPushUndefined();
						at = theStringifier->keys->value.reference;
					}
					else {
						at = fxNewInstance(the);
						mxBehaviorOwnKeys(the, anInstance, XS_EACH_NAME_FLAG, at);
					}
					run->instance = anInstance;
					run->at = at;
					run->ownFlag = 2;
					if (at->next) {
						theStringifier->level++;
						fxStringifyJSONIndent(the, theStringifier);
						mxPushUndefined();
						run->property = the->stack;
						mxPushReference(anInstance);
						run->phase = mxStringifyObjectPhase;
						continue;
					}
					mxPop();
					fxStringifyJSONChars(the, theStringifier, "}", 1);
					anInstance->flag &= ~XS_LEVEL_FLAG;
				}
			}
			else {
				/* stock: undefined/function/symbol element under an array */
				if (*flagCell & 4) {
					if (*flagCell & 1) {
						fxStringifyJSONChars(the, theStringifier, ",", 1);
						fxStringifyJSONIndent(the, theStringifier);
					}
					else
						*flagCell |= 1;
					fxStringifyJSONChars(the, theStringifier, "null", 4);
				}
			}
			mxPop(); // POP VALUE
			fxStringifyRunPop();
			continue;
		}

		case mxStringifyArrayPhase:
		for (;;) {
			if (run->index >= run->length) {
				theStringifier->level--;
				fxStringifyJSONIndent(the, theStringifier);
				fxStringifyJSONChars(the, theStringifier, "]", 1);
				run->instance->flag &= ~XS_LEVEL_FLAG;
				fxStringifyRunPop();
				break;
			}
			mxPushReference(run->instance);
			mxGetIndex(run->index);
			mxPushInteger((txInteger)run->index);
			run->index++;
			{
				txStringifyRun* child = fxStringifyRunPush(the);
				child->phase = mxStringifyHeadPhase;
				child->key = the->stack;
				child->flagAddress = &(run->ownFlag);
			}
			break;
		}
		continue;

		case mxStringifyObjectPhase:
		for (;;) {
			txSlot* anInstance = run->instance;
			txSlot* property = run->property;
			txSlot* at = run->at;
			int pushed = 0;
			while ((at = at->next)) {
				if (mxBehaviorGetOwnProperty(the, anInstance, at->value.at.id, at->value.at.index, property) && !(property->flag & XS_DONT_ENUM_FLAG)) {
					run->at = at;
					mxPushReference(anInstance);
					mxGetAll(at->value.at.id, at->value.at.index);
					if (at->value.at.id)
						fxPushKeyString(the, at->value.at.id, C_NULL);
					else
						mxPushInteger((txInteger)at->value.at.index);
					{
						txStringifyRun* child = fxStringifyRunPush(the);
						child->phase = mxStringifyHeadPhase;
						child->key = the->stack;
						child->flagAddress = &(run->ownFlag);
					}
					pushed = 1;
					break;
				}
			}
			if (pushed)
				break;
			run->at = at;
			mxPop();
			mxPop();
			theStringifier->level--;
			fxStringifyJSONIndent(the, theStringifier);
			fxStringifyJSONChars(the, theStringifier, "}", 1);
			run->instance->flag &= ~XS_LEVEL_FLAG;
			fxStringifyRunPop();
			break;
		}
		continue;
		}
	}
}


void fxStringifyJSONString(txMachine* the, txJSONStringifier* theStringifier, txString theString)
{
	fxStringifyJSONChars(the, theStringifier, "\"", 1);
	for (;;) {
		txInteger character;	
		theString = mxStringByteDecode(theString, &character);
		if (character == C_EOF)
			break;
		if (character < 8)
			fxStringifyJSONUnicodeEscape(the, theStringifier, character);
		else if (character == 8)
			fxStringifyJSONChars(the, theStringifier, "\\b", 2); 
		else if (character == 9)
			fxStringifyJSONChars(the, theStringifier, "\\t", 2); 
		else if (character == 10)
			fxStringifyJSONChars(the, theStringifier, "\\n", 2); 
		else if (character == 11)
			fxStringifyJSONUnicodeEscape(the, theStringifier, character); 
		else if (character == 12)
			fxStringifyJSONChars(the, theStringifier, "\\f", 2);
		else if (character == 13)
			fxStringifyJSONChars(the, theStringifier, "\\r", 2);
		else if (character < 32)
			fxStringifyJSONUnicodeEscape(the, theStringifier, character);
		else if (character < 34)
			fxStringifyJSONCharacter(the, theStringifier, character);
		else if (character == 34)
			fxStringifyJSONChars(the, theStringifier, "\\\"", 2);
		else if (character < 92)
			fxStringifyJSONCharacter(the, theStringifier, character);
		else if (character == 92)
			fxStringifyJSONChars(the, theStringifier, "\\\\", 2);
		else if (character < 127)
			fxStringifyJSONCharacter(the, theStringifier, character);
		else if ((0xD800 <= character) && (character <= 0xDFFF))
			fxStringifyJSONUnicodeEscape(the, theStringifier, character);
		else
			fxStringifyJSONCharacter(the, theStringifier, character);
	}
	fxStringifyJSONChars(the, theStringifier, "\"", 1);
}

void fxStringifyJSONUnicodeEscape(txMachine* the, txJSONStringifier* theStringifier, txInteger character)
{
	char buffer[16];
	txString p = buffer;
	*p++ = '\\'; 
	*p++ = 'u'; 
	p = fxStringifyUnicodeEscape(p, character, '\\');
	fxStringifyJSONChars(the, theStringifier, buffer, mxPtrDiff(p - buffer));
}

txSlot* fxToJSONKeys(txMachine* the, txSlot* reference)
{
	txSlot* list = fxNewInstance(the);
	txSlot* item = list;
	txSlot* slot;
	txIndex length, i;
	mxPushSlot(reference);
	mxGetID(mxID(_length));
	length = (txIndex)fxToLength(the, the->stack);
	mxPop();
	i = 0;
	while (i < length) {
		txBoolean flag = 0;
		txID id = XS_NO_ID;
		txIndex index = 0;
		mxPushSlot(reference);
		mxGetIndex(i);
		slot = the->stack;
	again:
		if ((slot->kind == XS_STRING_KIND) || (slot->kind == XS_STRING_X_KIND)) {
			if (fxStringToIndex(the, slot->value.string, &index))
				flag = 1;
			else {
				if (slot->kind == XS_STRING_X_KIND)
					id = fxNewNameX(the, slot->value.string);
				else
					id = fxNewName(the, slot);
				flag = 1;
			}
		}
		else if (slot->kind == XS_INTEGER_KIND) {
			if (fxIntegerToIndex(the, slot->value.integer, &index))
				flag = 1;
			else {
				fxToString(the, slot);
				goto again;
			}
		}
		else if (slot->kind == XS_NUMBER_KIND){
			if (fxNumberToIndex(the, slot->value.number, &index))
				flag = 1;
			else {
				fxToString(the, slot);
				goto again;
			}
		}
		else if (slot->kind == XS_REFERENCE_KIND) {
			txSlot* instance = slot->value.reference;
			if (mxIsNumber(instance) || mxIsString(instance)) {
				fxToString(the, slot);
				goto again;
			}
		}
		if (flag) {
			txSlot* already = list->next;
			while (already) {
				if ((already->value.at.id == id) && (already->value.at.index == index))
					break;
				already = already->next;
			}
			if (!already) {
				item = item->next = fxNewSlot(the);
				item->value.at.id = id;
				item->value.at.index = index;
				item->kind = XS_AT_KIND;
			}
		}
		mxPop();
		i++;
	}
	return the->stack;
}
