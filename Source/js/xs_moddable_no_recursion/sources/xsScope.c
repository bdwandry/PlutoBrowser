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

#include "xsScript.h"

#define mxBindHoistPart\
	txParser* parser;\
	txScope* scope

typedef struct sxExportlink txExportLink;

typedef struct {
	mxBindHoistPart;
	txInteger scopeLevel;
	txInteger scopeMaximum;
	txClassNode* classNode;
} txBinder;

typedef struct {
	mxBindHoistPart;
	txScope* functionScope;
	txScope* bodyScope;
	txNode* environmentNode;
	txExportLink* firstExportLink;
	txClassNode* classNode;
} txHoister;

struct sxExportlink {
	txExportLink* next;
	txSymbol* symbol;
};

static void fxHoisterAddExportLink(txHoister* self, txSpecifierNode* specifier);
static void fxBinderPopVariables(txBinder* self, txInteger count);
static void fxBinderPushVariables(txBinder* self, txInteger count);
static txScope* fxScopeNew(txHoister* hoister, txNode* node, txToken token);
static void fxScopeAddDeclareNode(txScope* self, txDeclareNode* node);
static void fxScopeAddDefineNode(txScope* self, txDefineNode* node);
static void fxScopeArrow(txScope* self);
static void fxScopeBindDefineNodes(txScope* self, void* param);
static void fxScopeBinding(txScope* self, txBinder* binder);
static void fxScopeBound(txScope* self, txBinder* binder);
static void fxScopeEval(txScope* self);
static txDeclareNode* fxScopeGetDeclareNode(txScope* self, txSymbol* symbol);
static void fxScopeHoisted(txScope* self, txHoister* hoister);
static void fxScopeLookup(txScope* self, txAccessNode* access, txBoolean closureFlag);

static void fxNodeDispatchBind(void* it, void* param);
static void fxNodeDispatchHoist(void* it, void* param);
static void fxFunctionNodeRename(void* it, txSymbol* symbol);
static void fxClassNodeHoistItems(txClassNode* self, txHoister* hoister);

/* xs_no_recursion: iterative hoist/bind walkers. Child visits are pushed
   onto a heap-resident frame stack (parser->nodeWalkStack) and drained
   iteratively, so native C depth stays constant regardless of AST depth.
   The per-node functions keep their stock names/addresses because the
   tree dispatch tables (xsTree.c) reference them. */

enum {
	W_DISPATCH_H = 1,
	W_DISPATCH_B,
	WH_BLOCK, WH_BODY, WH_CALL, WH_CATCH, WH_CLASS, WH_COALESCE, WH_DEFINE, WH_EXPORT,
	WH_FOR, WH_FORINFOROF, WH_FUNCTION, WH_MODULE, WH_PARAMS_BINDING, WH_PROGRAM,
	WH_SWITCH, WH_WITH,
	WB_ARRAY, WB_ARRAY_BINDING, WB_ASSIGN, WB_BINDING, WB_BLOCK, WB_CATCH, WB_CLASS,
	WB_DELEGATE, WB_FIELD, WB_FOR, WB_FORINFOROF, WB_FUNCTION, WB_MODULE, WB_OBJECT,
	WB_OBJECT_BINDING, WB_PARAMS, WB_PARAMS_BINDING, WB_POSTFIX, WB_PRIVATE_MEMBER,
	WB_PROGRAM, WB_SPREAD, WB_SUPER, WB_SWITCH, WB_TEMPLATE, WB_TRY, WB_WITH,
	WC_BASE	/* xs_no_recursion: code-emitter frame kinds start here (xsCode.c) */
};

typedef struct sxNodeWalkFrame txNodeWalkFrame;

#define sxNodeCollectMaxChildren 8

typedef struct sxNodeCollectItem sxNodeCollectItem;

struct sxNodeCollectItem {
	sxNodeCollectItem* next;
	txNode* node;
};

typedef struct {
	txParser* parser;
	sxNodeCollectItem* reversed;	/* children in reverse visit order */
} sxNodeCollectContext;

static void fxNodeCollectList(sxNodeCollectContext* context, txNode* first)
{
	txNode* node = first;
	while (node) {
		sxNodeCollectItem* item = (sxNodeCollectItem*)fxNewParserChunk(context->parser, sizeof(sxNodeCollectItem));
		item->node = node;
		item->next = context->reversed;
		context->reversed = item;
		node = node->next;
	}
}

static void fxNodeWalkPush(txParser* parser, void* it, void* param, int kind, int stage);
static void fxNodeWalkPop(txParser* parser);
static void fxNodeWalkStep(txParser* parser);
static void fxNodeDispatchHoistIter(void* it, void* param);
static void fxNodeDispatchBindIter(void* it, void* param);
void fxNodeWalkPushCode(txParser* parser, void* it, void* param, int kind, int stage);
void fxNodeWalkPopCode(txParser* parser);
void fxNodeWalkDrainCode(txParser* parser, void* baseFrame);
extern void fxNodeCodeBody(txParser* parser);
static void fxNodeHoistBody(txParser* parser);
static void fxNodeBindBody(txParser* parser);
static void fxNodeCollectVisit(void* it, void* param);

void fxParserBind(txParser* parser)
{
	txBinder binder;
	c_memset(&binder, 0, sizeof(txBinder));
	binder.parser = parser;
	if (parser->errorCount == 0) {
		mxTryParser(parser) {
			fxNodeDispatchBindIter(parser->root, &binder);
		}
		mxCatchParser(parser) {
			parser->nodeWalkStack = C_NULL;
			parser->nodeWalkRunning = 0;
		}
	}
	/* xs_no_recursion: the walker drains everything it pushes; a non-empty
	   stack here means a step returned without consuming its frame (which
	   would corrupt the LIFO contract). Detached builds crash-check it. */
	if (parser->nodeWalkStack != C_NULL) {
		parser->nodeWalkStack = C_NULL;
		parser->nodeWalkRunning = 0;
	}
}

void fxParserHoist(txParser* parser)
{
	txHoister hoister;
	c_memset(&hoister, 0, sizeof(txHoister));
	hoister.parser = parser;
	if (parser->errorCount == 0) {
		mxTryParser(parser) {
			fxNodeDispatchHoistIter(parser->root, &hoister);
		}
		mxCatchParser(parser) {
			parser->nodeWalkStack = C_NULL;
			parser->nodeWalkRunning = 0;
		}
	}
}

static void fxNodeWalkPush(txParser* parser, void* it, void* param, int kind, int stage)
{
	txNodeWalkFrame* frame = parser->nodeWalkPool;
	if (frame)
		parser->nodeWalkPool = frame->next;
	else
		frame = (txNodeWalkFrame*)fxNewParserChunkClear(parser, sizeof(txNodeWalkFrame));
	frame->next = parser->nodeWalkStack;
	frame->it = it;
	frame->param = param;
	frame->kind = kind;
	frame->stage = stage;
	frame->v0 = C_NULL;
	frame->v1 = C_NULL;
	frame->v2 = C_NULL;
	frame->v3 = C_NULL;
	frame->v4 = C_NULL;
	frame->v5 = C_NULL;
	frame->v6 = C_NULL;
	frame->v7 = C_NULL;
	frame->i0 = 0;
	frame->i1 = 0;
	frame->i2 = 0;
	frame->i3 = 0;
	frame->i4 = 0;
	frame->i5 = 0;
	frame->i6 = 0;
	frame->i7 = 0;
	parser->nodeWalkStack = frame;
}

static void fxNodeWalkPop(txParser* parser)
{
	txNodeWalkFrame* frame = parser->nodeWalkStack;
	parser->nodeWalkStack = frame->next;
	frame->next = parser->nodeWalkPool;
	frame->it = C_NULL;
	frame->param = C_NULL;
	frame->v0 = C_NULL;
	frame->v1 = C_NULL;
	frame->v2 = C_NULL;
	frame->v3 = C_NULL;
	parser->nodeWalkPool = frame;
}

/* xs_no_recursion: shared walker stack access for the code emitter
   (xsCode.c). Code-kind frames (WC_*) are routed through fxNodeWalkStep
   so hoist/bind and code emission share one pump. */
void fxNodeWalkPushCode(txParser* parser, void* it, void* param, int kind, int stage)
{
	fxNodeWalkPush(parser, it, param, kind, stage);
}

void fxNodeWalkPopCode(txParser* parser)
{
	fxNodeWalkPop(parser);
}

void fxNodeWalkDrainCode(txParser* parser, void* baseFrame)
{
	/* xs_no_recursion (R13): runaway watchdog. Every step must consume a
	   node or make progress; a machine that re-dispatches without popping
	   (or loops over a cursor that never advances) spins the frame loop
	   FOREVER on the device (observed: 502KB bundle compile hung past 9
	   minutes with no crashlog — the pump had no bound). 1e8 steps ≈ a
	   few seconds of device CPU even for the largest legal bundles; the
	   legitimate 502KB compile completes in ~50M steps on the sim. A trip
	   reports a compile error through the normal contained path (the
	   bridge logs "script failed: ..." and the page continues). */
	unsigned long guard = 100000000UL; /* XS_NR_DRAIN_LIMIT */
	while (parser->nodeWalkStack != (txNodeWalkFrame*)baseFrame) {
		if (--guard == 0) {
			fxReportParserError(parser, 0, "xs_no_recursion: pump runaway (drain limit)");
			return;
		}
		fxNodeWalkStep(parser);
	}
}

void* fxNodeWalkStackTop(txParser* parser)
{
	return parser->nodeWalkStack;
}

int fxNodeWalkRunningCheck(txParser* parser)
{
	return parser->nodeWalkRunning;
}

void fxNodeWalkSetRunningCode(txParser* parser, int value)
{
	parser->nodeWalkRunning = value;
}

void fxNodeWalkCallCode(txParser* parser, void* it, void* param, int kind, int stage, txInteger i0, txInteger i1)
{
	txNodeWalkFrame* base = parser->nodeWalkStack;
	int running = parser->nodeWalkRunning;
	unsigned long guard = 100000000UL; /* xs_no_recursion runaway bound, see fxNodeWalkDrainCode */
	parser->nodeWalkRunning = 1;
	fxNodeWalkPush(parser, it, param, kind, stage);
	parser->nodeWalkStack->i0 = i0;
	parser->nodeWalkStack->i1 = i1;
	while (parser->nodeWalkStack != base) {
		if (--guard == 0) {
			txNodeWalkFrame* f = parser->nodeWalkStack;
			fxReportParserError(parser, 0,
				"xs_no_recursion: code drain runaway (kind=%d stage=%d it=%p)",
				f ? f->kind : -1, f ? f->stage : -1, f ? f->it : NULL);
			return;
		}
		fxNodeWalkStep(parser);
	}
	parser->nodeWalkRunning = running;
}

static void fxNodeCollectVisit(void* it, void* param)
{
	sxNodeCollectContext* context = param;
	sxNodeCollectItem* item;
	if (!it)
		return;
	item = (sxNodeCollectItem*)fxNewParserChunk(context->parser, sizeof(sxNodeCollectItem));
	item->node = (txNode*)it;
	item->next = context->reversed;
	context->reversed = item;
}

static void fxNodeWalkPushReversed(txParser* parser, sxNodeCollectContext* context, void* param, int kind)
{
	sxNodeCollectItem* item = context->reversed;
	while (item) {
		fxNodeWalkPush(parser, item->node, param, kind, 0);
		item = item->next;
	}
}

void fxHoisterAddExportLink(txHoister* self, txSpecifierNode* specifier)
{
	txExportLink* link = self->firstExportLink;
	txSymbol* symbol = specifier->asSymbol ? specifier->asSymbol : specifier->symbol;
    if (symbol) {
        while (link) {
            if (link->symbol == symbol) {
                fxReportParserError(self->parser, specifier->line, "duplicate export %s", symbol->string);
                return;
            }
            link = link->next;
        }
        link = fxNewParserChunk(self->parser, sizeof(txExportLink));
        link->next = self->firstExportLink;
        link->symbol = symbol;
        self->firstExportLink = link;
    }
}

void fxBinderPopVariables(txBinder* self, txInteger count)
{
	self->scopeLevel -= count;
}

void fxBinderPushVariables(txBinder* self, txInteger count)
{
	self->scopeLevel += count;
	if (self->scopeMaximum < self->scopeLevel)
		self->scopeMaximum = self->scopeLevel;
}

txScope* fxScopeNew(txHoister* hoister, txNode* node, txToken token) 
{
	txScope* scope = fxNewParserChunkClear(hoister->parser, sizeof(txScope));
	scope->parser = hoister->parser;
	scope->scope = hoister->scope;
	scope->token = token;
	scope->flags = node->flags & mxStrictFlag;
	scope->node = node;
	hoister->scope = scope;
	return scope;
}

void fxScopeAddDeclareNode(txScope* self, txDeclareNode* node) 
{
	self->declareNodeCount++;
	if (self->token == XS_TOKEN_EVAL) {
		if (self->lastDeclareNode)
			node->nextDeclareNode = self->firstDeclareNode;
		else
			self->lastDeclareNode = node;
		self->firstDeclareNode = node;
	}
	else {
		if (self->lastDeclareNode)
			self->lastDeclareNode->nextDeclareNode = node;
		else
			self->firstDeclareNode = node;
		self->lastDeclareNode = node;
	}
	if (node->description->token == XS_TOKEN_USING) {
		txDeclareNode* node = fxDeclareNodeNew(self->parser, XS_TOKEN_CONST, C_NULL);
		node->flags |= mxDeclareNodeDisposableFlag;
		fxScopeAddDeclareNode(self, node);
		self->disposableNodeCount++;
	}
}

void fxScopeAddDefineNode(txScope* self, txDefineNode* node) 
{
	self->defineNodeCount++;
	if (self->lastDefineNode)
		self->lastDefineNode->nextDefineNode = node;
	else
		self->firstDefineNode = node;
	self->lastDefineNode = node;
}

void fxScopeArrow(txScope* self)
{
	/* xs_no_recursion: iterative scope-chain walk (was tail recursion) */
	while (self) {
		if (self->token == XS_TOKEN_EVAL)
			return;
		if (self->token == XS_TOKEN_FUNCTION) {
			if (self->node->flags & mxArrowFlag) {
				self->node->flags |= mxDefaultFlag;
				self = self->scope;
			}
			else
				return;
		}
		else if (self->token == XS_TOKEN_PROGRAM)
			return;
		else
			self = self->scope;
	}
}

void fxScopeBindDefineNodes(txScope* self, void* param) 
{
	txDefineNode* node = self->firstDefineNode;
	while (node) {
		fxNodeDispatchBindIter(node, param);
		node = node->nextDefineNode;
	}
}

void fxScopeBinding(txScope* self, txBinder* binder) 
{
	self->scope = binder->scope;
	binder->scope = self;
	fxBinderPushVariables(binder, self->declareNodeCount);
}

void fxScopeBound(txScope* self, txBinder* binder) 
{
	if (self->flags & mxEvalFlag) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			node->flags |= mxDeclareNodeClosureFlag;
			node = node->nextDeclareNode;
		}
	}
	if (self->token == XS_TOKEN_MODULE) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			if (!(node->flags & mxDeclareNodeDisposableFlag))
				node->flags |= mxDeclareNodeClosureFlag |  mxDeclareNodeUseClosureFlag;
			node = node->nextDeclareNode;
		}
	}
	else if (self->token == XS_TOKEN_PROGRAM) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			node->flags |= mxDeclareNodeClosureFlag |  mxDeclareNodeUseClosureFlag;
			node = node->nextDeclareNode;
		}
	}
	binder->scopeLevel += self->closureNodeCount;
	binder->scopeMaximum += self->closureNodeCount;
	fxBinderPopVariables(binder, self->declareNodeCount);
	binder->scope = self->scope;
}

void fxScopeEval(txScope* self) 
{
	while (self) {
		self->flags |= mxEvalFlag;
		self = self->scope;
	}
}

txDeclareNode* fxScopeGetDeclareNode(txScope* self, txSymbol* symbol) 
{
	txDeclareNode* node = self->firstDeclareNode;
	while (node) {
		if (node->symbol == symbol)
			return node;
		node = node->nextDeclareNode;
	}
	return NULL;
}

void fxScopeHoisted(txScope* self, txHoister* hoister) 
{
	if (self->token == XS_TOKEN_BLOCK) {
		txDeclareNode** address = &self->firstDeclareNode;
		txDeclareNode* node;
		txDeclareNode* last = C_NULL;
		while ((node = *address)) {
			if (node->description->token == XS_NO_TOKEN) {
				self->declareNodeCount--;
				*address = node->nextDeclareNode;
			}
			else {
				address = &node->nextDeclareNode;
				last = node;
			}
		}
		self->lastDeclareNode = last;
	}
	else if (self->token == XS_TOKEN_PROGRAM) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token == XS_TOKEN_DEFINE) || (node->description->token == XS_TOKEN_VAR))
				self->declareNodeCount--;
			node = node->nextDeclareNode;
		}
	}
	else if (self->token == XS_TOKEN_EVAL) {
		if (!(self->flags & mxStrictFlag)) {
			txDeclareNode* node = self->firstDeclareNode;
			while (node) {
				if ((node->description->token == XS_TOKEN_DEFINE) || (node->description->token == XS_TOKEN_VAR))
					self->declareNodeCount--;
				node = node->nextDeclareNode;
			}
		}
	}
	hoister->scope = self->scope;
}

typedef struct sxScopeWrap sxScopeWrap;

struct sxScopeWrap {
	sxScopeWrap* next;
	txScope* scope;
};

void fxScopeLookup(txScope* self, txAccessNode* access, txBoolean closureFlag) 
{
	/* xs_no_recursion: two-pass iterative scope walk. Pass 1 walks up the
	   scope chain (recording FUNCTION scopes that will need closure
	   wrapping) and resolves the declaration; pass 2 wraps it in closure
	   nodes from the outermost FUNCTION scope inward, exactly matching
	   the unwind order of the native-recursive original. */
	txDeclareNode* declaration;
	sxScopeWrap* wraps = C_NULL;
	txBoolean flag = closureFlag;
	txDeclareNode* resolved = C_NULL;
	int done = 0;
	int assign = 1;
	while (!done && self) {
		declaration = fxScopeGetDeclareNode(self, access->symbol);
		if (self->token == XS_TOKEN_EVAL) {
			if (declaration) {
				if ((!(self->flags & mxStrictFlag)) && ((declaration->description->token == XS_TOKEN_VAR) || (declaration->description->token == XS_TOKEN_DEFINE))) {
					declaration = C_NULL;
				}
				else if (flag)
					declaration->flags |= mxDeclareNodeClosureFlag;
			}
			else if ((self->flags & mxStrictFlag) && (access->description->token == XS_TOKEN_PRIVATE_MEMBER)) {
				declaration = fxDeclareNodeNew(self->parser, XS_TOKEN_PRIVATE, access->symbol);
				declaration->flags |= mxDeclareNodeClosureFlag;
				declaration->line = access->line;
				fxScopeAddDeclareNode(self, declaration);
				self->closureNodeCount++;
			}
			resolved = declaration;
			done = 1;
		}
		else if (self->token == XS_TOKEN_FUNCTION) {
			if (declaration) {
				if (flag)
					declaration->flags |= mxDeclareNodeClosureFlag;
				resolved = declaration;
				done = 1;
			}
			else if ((self->node->flags & mxEvalFlag) && !(self->node->flags & mxStrictFlag)) {
				// eval can create variables that override closures 
				resolved = C_NULL;
				done = 1;
			}
			else if (self->scope) {
				sxScopeWrap* wrap = (sxScopeWrap*)fxNewParserChunk(self->parser, sizeof(sxScopeWrap));
				wrap->scope = self;
				wrap->next = wraps;
				wraps = wrap;
				flag = 1;
				self = self->scope;
			}
			else {
				/* stock leaves access->declaration untouched in this case */
				assign = 0;
				done = 1;
			}
		}
		else if (self->token == XS_TOKEN_PROGRAM) {
			if (declaration && ((declaration->description->token == XS_TOKEN_VAR) || (declaration->description->token == XS_TOKEN_DEFINE))) {
				declaration = C_NULL;
			}
			resolved = declaration;
			done = 1;
		}
		else if (self->token == XS_TOKEN_WITH) {
			// with object can have properties that override variables 
			resolved = C_NULL;
			done = 1;
		}
		else {
			if (declaration) {
				if (flag)
					declaration->flags |= mxDeclareNodeClosureFlag;
				resolved = declaration;
				done = 1;
			}
			else if (self->scope) {
				self = self->scope;
			}
			else {
				resolved = C_NULL;
				access->symbol->usage |= 2;
				done = 1;
			}
		}
	}
	if (assign) {
		/* Prepending while walking outward already orders wrappers from
		   outermost to innermost, matching recursive return order. Each
		   inner proxy must refer to the proxy in its enclosing function. */
		sxScopeWrap* reversed = wraps;
		access->declaration = resolved;
		while (reversed) {
			/* stock checks access->declaration after each recursive return: a
			   NULL (e.g. a VAR/DEFINE nulled by the PROGRAM base case) stops the
			   closure-node chain there */
			if (access->declaration) {
				txDeclareNode* closureNode = fxDeclareNodeNew(reversed->scope->parser, XS_NO_TOKEN, access->symbol);
				closureNode->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
				closureNode->line = access->declaration->line;
				closureNode->declaration = access->declaration;
				fxScopeAddDeclareNode(reversed->scope, closureNode);
				reversed->scope->closureNodeCount++;
				access->declaration = closureNode;
			}
			reversed = reversed->next;
		}
	}
}

/* xs_no_recursion: host-only walk trace (opt-in; never compiled for device) */
static void fxNodeWalkStep(txParser* parser)
{
	txNodeWalkFrame* frame = parser->nodeWalkStack;
	if (frame->kind == W_DISPATCH_H) {
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchHoist(frame->it, frame->param);
			if (parser->nodeWalkStack == frame)
				fxNodeWalkPop(parser);	/* leaf visit: nothing pushed */
		}
		else
			fxNodeWalkPop(parser);	/* child visit completed */
	}
	else if (frame->kind == W_DISPATCH_B) {
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchBind(frame->it, frame->param);
			if (parser->nodeWalkStack == frame)
				fxNodeWalkPop(parser);
		}
		else
			fxNodeWalkPop(parser);
	}
	else if ((frame->kind >= WH_BLOCK) && (frame->kind <= WH_WITH))
		fxNodeHoistBody(parser);
	else if (frame->kind >= WC_BASE)
		fxNodeCodeBody(parser);
	else
		fxNodeBindBody(parser);
}

static void fxNodeDispatchHoistIter(void* it, void* param)
{
	txHoister* hoister = param;
	txNodeWalkFrame* base = hoister->parser->nodeWalkStack;
	int running = hoister->parser->nodeWalkRunning;
	unsigned long guard = 100000000UL; /* xs_no_recursion runaway bound, see fxNodeWalkDrainCode */
	hoister->parser->nodeWalkRunning = 1;
	fxNodeWalkPush(hoister->parser, it, param, W_DISPATCH_H, 0);
	while (hoister->parser->nodeWalkStack != base) {
		if (--guard == 0) {
			txNodeWalkFrame* f = hoister->parser->nodeWalkStack;
			fxReportParserError(hoister->parser, 0,
				"xs_no_recursion: hoist drain runaway (kind=%d stage=%d it=%p)",
				f ? f->kind : -1, f ? f->stage : -1, f ? f->it : NULL);
			return;
		}
		fxNodeWalkStep(hoister->parser);
	}
	hoister->parser->nodeWalkRunning = running;
}

void fxNodeDispatchHoist(void* it, void* param)
{
	txNode* node = it;
	fxCheckParserStack(((txHoister*)param)->parser, node->line);
	(*node->description->dispatch->hoist)(it, param);
}

void fxNodeHoist(void* it, void* param) 
{
	txNode* node = it;
	txHoister* hoister = param;
	sxNodeCollectContext context;
	context.parser = hoister->parser;
	context.reversed = C_NULL;
	(*node->description->dispatch->distribute)(node, fxNodeCollectVisit, &context);
	/* push in reverse so the LIFO stack visits children in stock order */
	fxNodeWalkPushReversed(hoister->parser, &context, param, W_DISPATCH_H);
}

void fxBlockNodeHoist(void* it, void* param) 
{
	txBlockNode* self = it;
	txHoister* hoister = param;
	self->scope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
	fxNodeWalkPush(hoister->parser, self, param, WH_BLOCK, 0);
	fxNodeWalkPush(hoister->parser, self->statement, param, W_DISPATCH_H, 0);
}

void fxBodyNodeHoist(void* it, void* param) 
{
	txBodyNode* self = it;
	txHoister* hoister = param;
	txNode* environmentNode = hoister->environmentNode;
	txNodeWalkFrame* frame;
	hoister->bodyScope = self->scope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
	hoister->environmentNode = it;
	fxNodeWalkPush(hoister->parser, self, param, WH_BODY, 0);
	frame = hoister->parser->nodeWalkStack;
	frame->stage = 1; /* XS_NR: 1 = statement not yet dispatched */
	frame->v0 = environmentNode;
	fxNodeWalkPush(hoister->parser, self->statement, param, W_DISPATCH_H, 0);
}

void fxCallNodeHoist(void* it, void* param) 
{
	txCallNewNode* self = it;
	txHoister* hoister = param;
	txParser* parser = hoister->parser;
	if (self->reference->description->token == XS_TOKEN_ACCESS) {
		txAccessNode* access = (txAccessNode*)self->reference;
		if (access->symbol == parser->evalSymbol) {
			fxScopeEval(hoister->scope);
			hoister->functionScope->node->flags |= mxArgumentsFlag | mxEvalFlag;
			hoister->environmentNode->flags |= mxEvalFlag;
			self->params->flags |= mxEvalParametersFlag;
		}
	}
	fxNodeWalkPush(parser, self, param, WH_CALL, 0);
	fxNodeWalkPush(parser, self->reference, param, W_DISPATCH_H, 0);
}

void fxCatchNodeHoist(void* it, void* param) 
{
	txCatchNode* self = it;
	txHoister* hoister = param;
	if (self->parameter) {
		self->scope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
		fxNodeWalkPush(hoister->parser, self, param, WH_CATCH, 0);
		fxNodeWalkPush(hoister->parser, self->parameter, param, W_DISPATCH_H, 0);
	}
	else {
		self->statementScope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
		fxNodeWalkPush(hoister->parser, self, param, WH_CATCH, 2);
		fxNodeWalkPush(hoister->parser, self->statement, param, W_DISPATCH_H, 0);
	}
}

void fxClassNodeHoist(void* it, void* param) 
{
	txClassNode* self = it;
	txHoister* hoister = param;
	txClassNode* former = hoister->classNode;
	txNodeWalkFrame* frame;
	if (self->heritage)
		fxNodeWalkPush(hoister->parser, self->heritage, param, W_DISPATCH_H, 0);
	if (self->symbol) {
		txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_CONST, self->symbol);
		node->flags |= mxDeclareNodeClosureFlag;
		self->symbolScope = fxScopeNew(hoister, it, XS_TOKEN_BLOCK);
		fxScopeAddDeclareNode(self->symbolScope, node);
	}
	self->scope = fxScopeNew(hoister, it, XS_TOKEN_BLOCK); /* XS_NR: scope exists before WH_CLASS items run */
	fxNodeWalkPush(hoister->parser, self, param, WH_CLASS, 0);
	frame = hoister->parser->nodeWalkStack;
	frame->v0 = former;
	frame->v1 = NULL;
}

static void fxClassNodeHoistItems(txClassNode* self, txHoister* hoister)
{
	txNode* item = self->items->first;
	while (item) {
		if (item->description->token == XS_TOKEN_PROPERTY) {
		}
		else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
			}
			else {
				txSymbol* symbol = fxNewParserChunkClear(hoister->parser, sizeof(txSymbol));
				txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_CONST, symbol);
				symbol->ID = -1;
				node->flags |= mxDeclareNodeClosureFlag;
				fxScopeAddDeclareNode(self->scope, node);
				((txPropertyAtNode*)item)->atAccess = fxAccessNodeNew(hoister->parser, XS_TOKEN_ACCESS, symbol);
			}
		}
		else {
			txSymbol* symbol = ((txPrivatePropertyNode*)item)->symbol;
			txDeclareNode* node = fxScopeGetDeclareNode(self->scope, symbol);
			if (node) {
                txUnsigned flags = (node->flags & (mxStaticFlag | mxGetterFlag | mxSetterFlag)) ^ (item->flags & (mxStaticFlag | mxGetterFlag | mxSetterFlag));
				if ((flags != (mxGetterFlag | mxSetterFlag)))
					fxReportParserError(hoister->parser, item->line, "duplicate %s", symbol->string);
			}
			node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_CONST, symbol);
			node->flags |= mxDeclareNodeClosureFlag | (item->flags & (mxStaticFlag | mxGetterFlag | mxSetterFlag));
			fxScopeAddDeclareNode(self->scope, node);
			((txPrivatePropertyNode*)item)->symbolAccess = fxAccessNodeNew(hoister->parser, XS_TOKEN_ACCESS, symbol);
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				txSymbol* symbol = fxNewParserChunkClear(hoister->parser, sizeof(txSymbol));
				txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_CONST, symbol);
				symbol->ID = -1;
				node->flags |= mxDeclareNodeClosureFlag;
				fxScopeAddDeclareNode(self->scope, node);
				((txPrivatePropertyNode*)item)->valueAccess = fxAccessNodeNew(hoister->parser, XS_TOKEN_ACCESS, symbol);
			}
		}
		item = item->next;
	}
	if (self->instanceInit) {
		txSymbol* symbol = fxNewParserChunkClear(hoister->parser, sizeof(txSymbol));
		txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_CONST, symbol);
		symbol->ID = -1;
		node->flags |= mxDeclareNodeClosureFlag;
		fxScopeAddDeclareNode(self->scope, node);
		self->instanceInitAccess = fxAccessNodeNew(hoister->parser, XS_TOKEN_ACCESS, symbol);
	}
}

void fxCoalesceExpressionNodeHoist(void* it, void* param) 
{
	txBinaryExpressionNode* self = it;
	txHoister* hoister = param;
	txToken leftToken = self->left->description->token;
	txToken rightToken = self->right->description->token;
	if ((leftToken == XS_TOKEN_AND) || (rightToken == XS_TOKEN_AND))
		fxReportParserError(hoister->parser, self->line, "missing () around &&");
	else if ((leftToken == XS_TOKEN_OR) || (rightToken == XS_TOKEN_OR))
		fxReportParserError(hoister->parser, self->line, "missing () around ||");
	fxNodeWalkPush(hoister->parser, self, param, WH_COALESCE, 0);
	fxNodeWalkPush(hoister->parser, self->left, param, W_DISPATCH_H, 0);
}

void fxDeclareNodeHoist(void* it, void* param) 
{
	txDeclareNode* self = it;
	txHoister* hoister = param;
	txDeclareNode* node;
	txScope* scope;
	if (self->description->token == XS_TOKEN_ARG) {
		node = fxScopeGetDeclareNode(hoister->functionScope, self->symbol);
		if (node) {
			if ((node->description->token == XS_TOKEN_ARG) && (hoister->functionScope->node->flags & (mxArrowFlag | mxAsyncFlag | mxMethodFlag | mxNotSimpleParametersFlag | mxStrictFlag)))
				fxReportParserError(hoister->parser, self->line, "duplicate argument %s", self->symbol->string);
		}
		else {
			fxScopeAddDeclareNode(hoister->functionScope, self);
		}
	}
	else if ((self->description->token == XS_TOKEN_CONST) || (self->description->token == XS_TOKEN_LET) || (self->description->token == XS_TOKEN_USING)) {
		node = fxScopeGetDeclareNode(hoister->scope, self->symbol);
		if (!node && (hoister->scope == hoister->bodyScope)) {
			node = fxScopeGetDeclareNode(hoister->functionScope, self->symbol);
			if (node && (node->description->token != XS_TOKEN_ARG))
				node = C_NULL;
		}
		if (node)
			fxReportParserError(hoister->parser, self->line, "duplicate variable %s", self->symbol->string);
		else
			fxScopeAddDeclareNode(hoister->scope, self);
	}
	else {
		scope = hoister->scope;
		node = C_NULL;
		while (scope != hoister->bodyScope) {
			node = fxScopeGetDeclareNode(scope, self->symbol);
			if (node) {
				if ((node->description->token == XS_TOKEN_CONST) || (node->description->token == XS_TOKEN_LET) || (node->description->token == XS_TOKEN_USING) || (node->description->token == XS_TOKEN_DEFINE))
					break;
				node = C_NULL;
			}
			scope = scope->scope;
		}
		if (!node) {
			node = fxScopeGetDeclareNode(scope, self->symbol);
			if (node) {
				if ((node->description->token != XS_TOKEN_CONST) && (node->description->token != XS_TOKEN_LET) && (node->description->token != XS_TOKEN_USING))
					node = C_NULL;
			}
		}
		if (node)
			fxReportParserError(hoister->parser, self->line, "duplicate variable %s", self->symbol->string);
		else {
			node = fxScopeGetDeclareNode(hoister->functionScope, self->symbol);
			if (!node || ((node->description->token != XS_TOKEN_ARG) && (node->description->token != XS_TOKEN_VAR)))
				fxScopeAddDeclareNode(hoister->bodyScope, self);
			scope = hoister->scope;
			while (scope != hoister->bodyScope) {
				fxScopeAddDeclareNode(scope, fxDeclareNodeNew(hoister->parser, XS_NO_TOKEN, self->symbol));
				scope = scope->scope;
			}
		}
	}
}

void fxDefineNodeHoist(void* it, void* param) 
{
	txDefineNode* self = it;
	txHoister* hoister = param;
	txDeclareNode* node;
	if (self->flags & mxStrictFlag) {
		if ((self->symbol == hoister->parser->argumentsSymbol) || (self->symbol == hoister->parser->evalSymbol) || (self->symbol == hoister->parser->yieldSymbol))
			fxReportParserError(hoister->parser, self->line, "invalid definition %s", self->symbol->string);
	}
	if ((hoister->scope == hoister->bodyScope) && (hoister->scope->token != XS_TOKEN_MODULE)) {
		node = fxScopeGetDeclareNode(hoister->bodyScope, self->symbol);
		if (node) {
			if ((node->description->token == XS_TOKEN_CONST) || (node->description->token == XS_TOKEN_LET))
				fxReportParserError(hoister->parser, self->line, "duplicate variable %s", self->symbol->string);
		}
		else {
			if (hoister->functionScope != hoister->bodyScope)
				node = fxScopeGetDeclareNode(hoister->functionScope, self->symbol);
			if (!node)
				fxScopeAddDeclareNode(hoister->bodyScope, (txDeclareNode*)self);
		}
		fxScopeAddDefineNode(hoister->bodyScope, self);
	}
	else {
		node = fxScopeGetDeclareNode(hoister->scope, self->symbol);
		if (node)
			fxReportParserError(hoister->parser, self->line, "duplicate variable %s", self->symbol->string);
		else
			fxScopeAddDeclareNode(hoister->scope, (txDeclareNode*)self);
		fxScopeAddDefineNode(hoister->scope, self);
	}
	((txFunctionNode*)(self->initializer))->symbol = C_NULL;
	fxNodeWalkPush(hoister->parser, self, param, WH_DEFINE, 0);
	fxNodeWalkPush(hoister->parser, self->initializer, param, W_DISPATCH_H, 0);
}

void fxExportNodeHoist(void* it, void* param)
{
	txExportNode* self = it;
	txHoister* hoister = param;
	if (self->from) {
		if (self->specifiers && self->specifiers->length) {
			txSpecifierNode* specifier = (txSpecifierNode*)self->specifiers->first;
			while (specifier) {
				txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_LET, C_NULL);
				specifier->from = self->from;
				specifier->with = self->with;
				node->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
				node->line = self->line;
				node->importSpecifier = specifier;
				node->firstExportSpecifier = specifier;
				fxScopeAddDeclareNode(hoister->scope, node);
				specifier = (txSpecifierNode*)specifier->next;
			}
		}
		else {
			txSpecifierNode* specifier = fxSpecifierNodeNew(hoister->parser, XS_TOKEN_SPECIFIER);
			txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_LET, C_NULL);
			specifier->from = self->from;
			specifier->with = self->with;
			node->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
			node->line = self->line;
			node->importSpecifier = specifier;
			node->firstExportSpecifier = C_NULL;
			fxScopeAddDeclareNode(hoister->scope, node);
		}
	}
	if (self->specifiers && self->specifiers->length) {
		sxNodeCollectContext context;
		txSpecifierNode* specifier = (txSpecifierNode*)self->specifiers->first;
		context.parser = hoister->parser;
		context.reversed = C_NULL;
		while (specifier) {
			fxHoisterAddExportLink(hoister, specifier);
			fxNodeCollectVisit(specifier, &context);
			specifier = (txSpecifierNode*)specifier->next;
		}
		fxNodeWalkPushReversed(hoister->parser, &context, param, W_DISPATCH_H);
	}
}

void fxForNodeHoist(void* it, void* param) 
{
	txForNode* self = it;
	txHoister* hoister = param;
	self->scope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
	fxNodeWalkPush(hoister->parser, self, param, WH_FOR, 0);
	if (self->initialization)
		fxNodeWalkPush(hoister->parser, self->initialization, param, W_DISPATCH_H, 0);
}

void fxForInForOfNodeHoist(void* it, void* param) 
{
	txForInForOfNode* self = it;
	txHoister* hoister = param;
	self->scope = fxScopeNew(param, it, XS_TOKEN_BLOCK);
	fxNodeWalkPush(hoister->parser, self, param, WH_FORINFOROF, 0);
	fxNodeWalkPush(hoister->parser, self->reference, param, W_DISPATCH_H, 0);
}

void fxFunctionNodeHoist(void* it, void* param) 
{
	txFunctionNode* self = it;
	txHoister* hoister = param;
	txScope* functionScope = hoister->functionScope;
	txScope* bodyScope = hoister->bodyScope;
	txNodeWalkFrame* frame;
	hoister->functionScope = self->scope = fxScopeNew(param, it, XS_TOKEN_FUNCTION);
	hoister->bodyScope = C_NULL;
	if (self->symbol) {
		txDefineNode* node = fxDefineNodeNew(hoister->parser, XS_TOKEN_CONST, self->symbol);
		node->initializer = fxValueNodeNew(hoister->parser, XS_TOKEN_CURRENT);
		fxScopeAddDeclareNode(hoister->functionScope, (txDeclareNode*)node);
		fxScopeAddDefineNode(hoister->functionScope, node);
	}
	fxNodeWalkPush(hoister->parser, self, param, WH_FUNCTION, 0);
	frame = hoister->parser->nodeWalkStack;
	frame->v0 = functionScope;
	frame->v1 = bodyScope;
	fxNodeWalkPush(hoister->parser, self->params, param, W_DISPATCH_H, 0);
}

txHostNode* fxHostNodeClone(txParser* parser, txHostNode* self)
{
	txHostNode* node = fxNewParserChunkClear(parser, sizeof(txHostNode));
	c_memcpy(node, self, sizeof(txHostNode));
	return node;
}

void fxHostNodeHoist(void* it, void* param) 
{
	txHostNode* self = it;
	txHoister* hoister = param;
	txScope* scope = hoister->bodyScope;
	self->hostIndex = -1;
	if ((scope->token != XS_TOKEN_MODULE) && (scope->token != XS_TOKEN_PROGRAM)) {
		txParser* parser = hoister->parser;
		while ((scope->token != XS_TOKEN_MODULE) && (scope->token != XS_TOKEN_PROGRAM))
			scope = scope->scope;
		snprintf(parser->buffer, parser->bufferSize, "@%s", self->at->value);
		txSymbol* symbol = fxNewParserSymbol(parser, parser->buffer);
		if (!fxScopeGetDeclareNode(scope, symbol)) {
			txDefineNode* definition = fxDefineNodeNew(parser, XS_TOKEN_DEFINE, symbol);
			definition->initializer = (txNode*)fxHostNodeClone(parser, self);
			fxScopeAddDeclareNode(scope, (txDeclareNode*)definition);
			fxScopeAddDefineNode(scope, definition);
		}
		txAccessNode* access = it;
		access->description = &gxTokenDescriptions[XS_TOKEN_ACCESS];
		access->symbol = symbol;
		access->initializer = C_NULL;
		access->declaration = C_NULL;
	}
}

void fxImportNodeHoist(void* it, void* param) 
{
	txImportNode* self = it;
	txHoister* hoister = param;
	if (self->specifiers && self->specifiers->length) {
		txSpecifierNode* specifier = (txSpecifierNode*)self->specifiers->first;
		while (specifier) {
			txDeclareNode* node;
			txSymbol* symbol = specifier->asSymbol ? specifier->asSymbol : specifier->symbol;
			if (self->flags & mxStrictFlag) {
				if ((symbol == hoister->parser->argumentsSymbol) || (symbol == hoister->parser->evalSymbol))
					fxReportParserError(hoister->parser, self->line, "invalid import %s", symbol->string);
			}
			node = fxScopeGetDeclareNode(hoister->scope, symbol);
			if (node)
				fxReportParserError(hoister->parser, self->line, "duplicate variable %s", symbol->string);
			else {
				specifier->declaration = node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_LET, symbol);
				specifier->from = self->from;
				specifier->with = self->with;
				node->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
				node->line = self->line;
				node->importSpecifier = specifier;
				fxScopeAddDeclareNode(hoister->scope, node);
			}
			specifier = (txSpecifierNode*)specifier->next;
		}
	}
	else {
		txSpecifierNode* specifier = fxSpecifierNodeNew(hoister->parser, XS_TOKEN_SPECIFIER);
		txDeclareNode* node = fxDeclareNodeNew(hoister->parser, XS_TOKEN_LET, C_NULL);
		specifier->from = self->from;
		specifier->with = self->with;
		node->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
		node->line = self->line;
		node->importSpecifier = specifier;
		fxScopeAddDeclareNode(hoister->scope, node);
	}
}

void fxModuleNodeHoist(void* it, void* param) 
{
	txModuleNode* self = it;
	txHoister* hoister = param;
	hoister->functionScope = hoister->bodyScope = self->scope = fxScopeNew(param, it, XS_TOKEN_MODULE); // @@
	hoister->environmentNode = it;
	fxNodeWalkPush(hoister->parser, self, param, WH_MODULE, 0);
	fxNodeWalkPush(hoister->parser, self->body, param, W_DISPATCH_H, 0);
}

void fxParamsBindingNodeHoist(void* it, void* param)
{
	txParamsNode* self = it;
	txHoister* hoister = param;
	sxNodeCollectContext context;
	context.parser = hoister->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPushReversed(hoister->parser, &context, param, W_DISPATCH_H);
}

void fxProgramNodeHoist(void* it, void* param) 
{
	txProgramNode* self = it;
	txHoister* hoister = param;
	hoister->functionScope = hoister->bodyScope = self->scope = fxScopeNew(param, it, (hoister->parser->flags & mxEvalFlag) ? XS_TOKEN_EVAL : XS_TOKEN_PROGRAM);
	hoister->environmentNode = it;
	fxNodeWalkPush(hoister->parser, self, param, WH_PROGRAM, 0);
	fxNodeWalkPush(hoister->parser, self->body, param, W_DISPATCH_H, 0);
}

void fxStatementNodeHoist(void* it, void* param) 
{
	txStatementNode* self = it;
	fxNodeWalkPush(((txHoister*)param)->parser, self->expression, param, W_DISPATCH_H, 0);
}

void fxPropertyNodeHoist(void* it, void* param) 
{
	txPropertyNode* self = it;
	if (self->value) {
		if (self->value->description->token == XS_TOKEN_HOST)
			((txHostNode*)(self->value))->symbol = self->symbol;
		fxNodeWalkPush(((txHoister*)param)->parser, self->value, param, W_DISPATCH_H, 0);
	}
}

void fxStringNodeHoist(void* it, void* param)
{
	txStringNode* self = it;
	txHoister* hoister = param;
	if ((self->flags & mxStringLegacyFlag) && (hoister->scope->flags & mxStrictFlag))
		self->flags |= mxStringErrorFlag;
}

void fxSwitchNodeHoist(void* it, void* param) 
{
	txSwitchNode* self = it;
	txHoister* hoister = param;
	fxNodeWalkPush(hoister->parser, self, param, WH_SWITCH, 0);
	fxNodeWalkPush(hoister->parser, self->expression, param, W_DISPATCH_H, 0);
}

void fxWithNodeHoist(void* it, void* param) 
{
	txWithNode* self = it;
	txHoister* hoister = param;
	fxNodeWalkPush(hoister->parser, self, param, WH_WITH, 0);
	fxNodeWalkPush(hoister->parser, self->expression, param, W_DISPATCH_H, 0);
}

static void fxNodeDispatchBindIter(void* it, void* param)
{
	txBinder* binder = param;
	txNodeWalkFrame* base = binder->parser->nodeWalkStack;
	int running = binder->parser->nodeWalkRunning;
	unsigned long guard = 100000000UL; /* xs_no_recursion runaway bound, see fxNodeWalkDrainCode */
	binder->parser->nodeWalkRunning = 1;
	fxNodeWalkPush(binder->parser, it, param, W_DISPATCH_B, 0);
	while (binder->parser->nodeWalkStack != base) {
		if (--guard == 0) {
			txNodeWalkFrame* f = binder->parser->nodeWalkStack;
			fxReportParserError(binder->parser, 0,
				"xs_no_recursion: bind drain runaway (kind=%d stage=%d it=%p)",
				f ? f->kind : -1, f ? f->stage : -1, f ? f->it : NULL);
			return;
		}
		fxNodeWalkStep(binder->parser);
	}
	binder->parser->nodeWalkRunning = running;
}

void fxNodeDispatchBind(void* it, void* param)
{
	txNode* node = it;
	fxCheckParserStack(((txBinder*)param)->parser, node->line);
	(*node->description->dispatch->bind)(it, param);
}

void fxNodeBind(void* it, void* param) 
{
	txNode* node = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	context.parser = binder->parser;
	context.reversed = C_NULL;
	(*node->description->dispatch->distribute)(node, fxNodeCollectVisit, &context);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxAccessNodeBind(void* it, void* param) 
{
	txAccessNode* self = it;
	txBinder* binder = param;
	fxScopeLookup(binder->scope, (txAccessNode*)self, 0);
}

void fxArrayNodeBind(void* it, void* param) 
{
	txArrayNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxBinderPushVariables(param, 1);
	if (self->flags & mxSpreadFlag)
		fxBinderPushVariables(param, 2);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_ARRAY, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxArrayBindingNodeBind(void* it, void* param) 
{
	txArrayBindingNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxBinderPushVariables(param, 6);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_ARRAY_BINDING, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxAssignNodeBind(void* it, void* param) 
{
	txAssignNode* self = it;
	txBinder* binder = param;
	txNodeWalkFrame* frame;
	fxNodeWalkPush(binder->parser, self, param, WB_ASSIGN, 0);
	frame = binder->parser->nodeWalkStack;
	frame->i0 = self->reference->description->token;
	fxNodeWalkPush(binder->parser, self->reference, param, W_DISPATCH_B, 0);
}

void fxBindingNodeBind(void* it, void* param) 
{
	txBindingNode* self = it;
	txBinder* binder = param;
	txNodeWalkFrame* frame;
	fxNodeWalkPush(binder->parser, self, param, WB_BINDING, 0);
	frame = binder->parser->nodeWalkStack;
	frame->i0 = self->target->description->token;
	fxNodeWalkPush(binder->parser, self->target, param, W_DISPATCH_B, 0);
}

void fxBlockNodeBind(void* it, void* param) 
{
	txBlockNode* self = it;
	txBinder* binder = param;
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxBinderPushVariables(param, 2);
	fxNodeWalkPush(binder->parser, self, param, WB_BLOCK, 0);
	fxNodeWalkPush(binder->parser, self->statement, param, W_DISPATCH_B, 0);
}

void fxCatchNodeBind(void* it, void* param) 
{
	txCatchNode* self = it;
	txBinder* binder = param;
	txNodeWalkFrame* frame;
	fxNodeWalkPush(binder->parser, self, param, WB_CATCH, 0);
	frame = binder->parser->nodeWalkStack;
	if (self->parameter) {
		frame->i0 = 1;
		fxScopeBinding(self->scope, param);
		fxNodeWalkPush(binder->parser, self->parameter, param, W_DISPATCH_B, 0);
	}
	else {
		frame->i0 = 0;
		fxNodeWalkPush(binder->parser, self->statement, param, W_DISPATCH_B, 0);
	}
}

void fxClassNodeBind(void* it, void* param) 
{
	txClassNode* self = it;
	txBinder* binder = param;
	txNodeWalkFrame* frame;
	fxBinderPushVariables(param, 2);
	if (self->symbol)
		fxScopeBinding(self->symbolScope, param);
	fxNodeWalkPush(binder->parser, self, param, WB_CLASS, 0);
	frame = binder->parser->nodeWalkStack;
	frame->v0 = (void*)binder->classNode;
	frame->v1 = NULL;
	if (self->heritage)
		fxNodeWalkPush(binder->parser, self->heritage, param, W_DISPATCH_B, 0);
}

void fxDeclareNodeBind(void* it, void* param) 
{
	txBindingNode* self = it;
	txBinder* binder = param;
	fxScopeLookup(binder->scope, (txAccessNode*)self, 0);
}

void fxDefineNodeBind(void* it, void* param) 
{
	txDefineNode* self = it;
	txBinder* binder = param;
	if (self->flags & mxDefineNodeBoundFlag)
		return;
	self->flags |= mxDefineNodeBoundFlag;
	fxScopeLookup(binder->scope, (txAccessNode*)self, 0);
	fxNodeWalkPush(binder->parser, self->initializer, param, W_DISPATCH_B, 0);
}

void fxDelegateNodeBind(void* it, void* param) 
{
	txStatementNode* self = it;
	txBinder* binder = param;
	fxBinderPushVariables(param, 5);
	fxNodeWalkPush(binder->parser, self, param, WB_DELEGATE, 0);
	fxNodeWalkPush(binder->parser, self->expression, param, W_DISPATCH_B, 0);
}

void fxExportNodeBind(void* it, void* param) 
{
	txExportNode* self = it;
	txBinder* binder = param;
	if (self->from)
		return;
	if (self->specifiers) {
		txSpecifierNode* specifier = (txSpecifierNode*)self->specifiers->first;
		while (specifier) {
			txAccessNode* node = fxAccessNodeNew(binder->parser, XS_TOKEN_ACCESS, specifier->symbol);
			fxScopeLookup(binder->scope, node, 0);
			if (node->declaration) {
				specifier->declaration = node->declaration;
				specifier->declaration->flags |= mxDeclareNodeClosureFlag | mxDeclareNodeUseClosureFlag;
				specifier->nextSpecifier = specifier->declaration->firstExportSpecifier;
				specifier->declaration->firstExportSpecifier = specifier;
			}
			else
				fxReportParserError(binder->parser, specifier->line, "unknown variable %s", specifier->symbol->string);
			specifier = (txSpecifierNode*)specifier->next;
		}
	}
}

void fxFieldNodeBind(void* it, void* param) 
{
	txFieldNode* self = it;
	txBinder* binder = param;
	txNode* item = self->item;
	if (item->description->token == XS_TOKEN_PROPERTY_AT)
		fxScopeLookup(binder->scope, ((txPropertyAtNode*)item)->atAccess, 0);
	else if (item->description->token == XS_TOKEN_PRIVATE_PROPERTY) {
		if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag))
			fxScopeLookup(binder->scope, ((txPrivatePropertyNode*)item)->valueAccess, 0);
		fxScopeLookup(binder->scope, ((txPrivatePropertyNode*)item)->symbolAccess, 0);
	}
	if (self->value)
		fxNodeWalkPush(binder->parser, self->value, param, W_DISPATCH_B, 0);
}

void fxForNodeBind(void* it, void* param) 
{
	txForNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxBinderPushVariables(param, 2);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	/* collect children in stock visit order */
	if (self->initialization)
		fxNodeCollectVisit(self->initialization, &context);
	if (self->expression)
		fxNodeCollectVisit(self->expression, &context);
	if (self->iteration)
		fxNodeCollectVisit(self->iteration, &context);
	fxNodeCollectVisit(self->statement, &context);
	fxNodeWalkPush(binder->parser, self, param, WB_FOR, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxForInForOfNodeBind(void* it, void* param) 
{
	txForInForOfNode* self = it;
	txBinder* binder = param;
	fxBinderPushVariables(param, 6);
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	fxNodeWalkPush(binder->parser, self, param, WB_FORINFOROF, 0);
	fxNodeWalkPush(binder->parser, self->statement, param, W_DISPATCH_B, 0);
	fxNodeWalkPush(binder->parser, self->expression, param, W_DISPATCH_B, 0);
	fxNodeWalkPush(binder->parser, self->reference, param, W_DISPATCH_B, 0);
}

void fxFunctionNodeBind(void* it, void* param) 
{
	txFunctionNode* self = it;
	txBinder* binder = param;
	txInteger scopeLevel = binder->scopeLevel;
	txInteger scopeMaximum = binder->scopeMaximum;
	txNodeWalkFrame* frame;
	binder->scopeLevel = 0;
	binder->scopeMaximum = 0;
	fxScopeBinding(self->scope, param);
	fxNodeWalkPush(binder->parser, self, param, WB_FUNCTION, 0);
	frame = binder->parser->nodeWalkStack;
	frame->i0 = scopeLevel;
	frame->i1 = scopeMaximum;
	fxNodeWalkPush(binder->parser, self->params, param, W_DISPATCH_B, 0);
}

void fxFunctionNodeRename(void* it, txSymbol* symbol)
{
	txNode* self = it;
	txToken token = self->description->token;
	if (token == XS_TOKEN_EXPRESSIONS) {
		self = ((txExpressionsNode*)self)->items->first;
		if (self->next)
			return;
		token = self->description->token;
	}
	if (token == XS_TOKEN_CLASS) {
		txClassNode* node = (txClassNode*)self;
		if (!node->symbol)
			((txFunctionNode*)(node->constructor))->symbol = symbol;
	}
	else if ((token == XS_TOKEN_FUNCTION) || (token == XS_TOKEN_GENERATOR) || (token == XS_TOKEN_HOST)) {
		txFunctionNode* node = (txFunctionNode*)self;
		if (!node->symbol)
			node->symbol = symbol;
	}
}

void fxHostNodeBind(void* it, void* param) 
{
}

void fxModuleNodeBind(void* it, void* param) 
{
	txModuleNode* self = it;
	txBinder* binder = param;
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxBinderPushVariables(param, 2);
	fxNodeWalkPush(binder->parser, self, param, WB_MODULE, 0);
	fxNodeWalkPush(binder->parser, self->body, param, W_DISPATCH_B, 0);
}

void fxObjectNodeBind(void* it, void* param) 
{
	txObjectNode* self = it;
	txBinder* binder = param;
	txNode* item = self->items->first;
	sxNodeCollectContext context;
	fxBinderPushVariables(param, 1);
	while (item) {
		txNode* value;
		if (item->description->token == XS_TOKEN_SPREAD) {
		}
		else {
			if (item->description->token == XS_TOKEN_PROPERTY) {
				value = ((txPropertyNode*)item)->value;
			}
			else {
				value = ((txPropertyAtNode*)item)->value;
			}
			if ((value->description->token == XS_TOKEN_FUNCTION) || (value->description->token == XS_TOKEN_GENERATOR) || (value->description->token == XS_TOKEN_HOST)) {
				txFunctionNode* node = (txFunctionNode*)value;
				node->flags |= item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag);
			}
			else if (value->description->token == XS_TOKEN_CLASS) {
//				txFunctionNode* node = (txFunctionNode*)(((txClassNode*)value)->constructor);
			}
		}
		item = item->next;
	}
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_OBJECT, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxObjectBindingNodeBind(void* it, void* param) 
{
	txObjectBindingNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxBinderPushVariables(param, 2);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_OBJECT_BINDING, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxParamsNodeBind(void* it, void* param) 
{
	txParamsNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	if (self->flags & mxSpreadFlag)
		fxBinderPushVariables(param, 1);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_PARAMS, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxParamsBindingNodeBind(void* it, void* param) 
{
	txParamsBindingNode* self = it;
	txBinder* binder = param;
	txScope* functionScope = binder->scope;
	txFunctionNode* functionNode = (txFunctionNode*)(functionScope->node);
	txInteger count = self->items->length;
	if (functionNode->flags & mxGetterFlag) {
		if (count != 0)
			fxReportParserError(binder->parser, self->line, "invalid getter arguments");
	}
	else if (functionNode->flags & mxSetterFlag) {
		if ((count != 1) || (self->items->first->description->token == XS_TOKEN_REST_BINDING))
			fxReportParserError(binder->parser, self->line, "invalid setter arguments");
	}
	else {
		if (count > 255)
			fxReportParserError(binder->parser, self->line, "too many arguments");
	}
	if (functionNode->flags & mxArgumentsFlag) {
		txNode* item;
		self->declaration = fxScopeGetDeclareNode(functionScope, binder->parser->argumentsSymbol);
		if (functionNode->flags & mxStrictFlag)
			goto bail;
		item = self->items->first;
		while (item) {
			if (item->description->token != XS_TOKEN_ARG)
				goto bail;
			item = item->next;
		}
		item = self->items->first;
		while (item) {
			((txDeclareNode*)item)->flags |= mxDeclareNodeClosureFlag;
			item = item->next;
		}
		self->mapped = 1;
	}
bail:
	{
		sxNodeCollectContext context;
		context.parser = binder->parser;
		context.reversed = C_NULL;
		fxNodeCollectList(&context, self->items->first);
		fxNodeWalkPush(binder->parser, self, param, WB_PARAMS_BINDING, 0);
		fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
	}
}

void fxPostfixExpressionNodeBind(void* it, void* param) 
{
	txPostfixExpressionNode* self = it;
	txBinder* binder = param;
	fxNodeWalkPush(binder->parser, self, param, WB_POSTFIX, 0);
	fxNodeWalkPush(binder->parser, self->left, param, W_DISPATCH_B, 0);
}

void fxPrivateMemberNodeBind(void* it, void* param) 
{
	txBinder* binder = param;
	txPrivateMemberNode* self = it;
	fxScopeLookup(binder->scope, (txAccessNode*)self, 0);
	if (!self->declaration)
		fxReportParserError(binder->parser, self->line, "invalid private identifier");
	fxNodeWalkPush(binder->parser, self->reference, param, W_DISPATCH_B, 0);
}

void fxProgramNodeBind(void* it, void* param) 
{
	txProgramNode* self = it;
	txBinder* binder = param;
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	fxNodeWalkPush(binder->parser, self, param, WB_PROGRAM, 0);
	fxNodeWalkPush(binder->parser, self->body, param, W_DISPATCH_B, 0);
}

void fxSpreadNodeBind(void* it, void* param) 
{
	txSpreadNode* self = it;
	txBinder* binder = param;
	fxBinderPushVariables(param, 1);
	fxNodeWalkPush(binder->parser, self, param, WB_SPREAD, 0);
	fxNodeWalkPush(binder->parser, self->expression, param, W_DISPATCH_B, 0);
}

void fxSuperNodeBind(void* it, void* param)
{
	txSuperNode* self = it;
	txBinder* binder = param;
	txNodeWalkFrame* frame;
	fxNodeWalkPush(binder->parser, self, param, WB_SUPER, 0);
	frame = binder->parser->nodeWalkStack;
	fxScopeArrow(binder->scope);
	fxNodeWalkPush(binder->parser, self->params, param, W_DISPATCH_B, 0);
}

void fxSwitchNodeBind(void* it, void* param) 
{
	txSwitchNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxScopeBinding(self->scope, param);
	fxScopeBindDefineNodes(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxBinderPushVariables(param, 2);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectList(&context, self->items->first);
	/* LIFO: frame deepest, then items, expression last (visited first) */
	fxNodeWalkPush(binder->parser, self, param, WB_SWITCH, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
	fxNodeWalkPush(binder->parser, self->expression, param, W_DISPATCH_B, 0);
}

void fxTargetNodeBind(void* it, void* param)
{
	txBinder* binder = param;
	fxScopeArrow(binder->scope);
}

void fxTemplateNodeBind(void* it, void* param) 
{
	txTemplateNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	if (self->reference)
		fxBinderPushVariables(param, 2);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	if (self->reference)
		fxNodeCollectVisit(self->reference, &context);
	fxNodeCollectList(&context, self->items->first);
	fxNodeWalkPush(binder->parser, self, param, WB_TEMPLATE, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxThisNodeBind(void* it, void* param)
{
	txBinder* binder = param;
	fxScopeArrow(binder->scope);
}

void fxTryNodeBind(void* it, void* param) 
{
	txTryNode* self = it;
	txBinder* binder = param;
	sxNodeCollectContext context;
	fxBinderPushVariables(param, 3);
	context.parser = binder->parser;
	context.reversed = C_NULL;
	fxNodeCollectVisit(self->tryBlock, &context);
	if (self->catchBlock)
		fxNodeCollectVisit(self->catchBlock, &context);
	if (self->finallyBlock)
		fxNodeCollectVisit(self->finallyBlock, &context);
	fxNodeWalkPush(binder->parser, self, param, WB_TRY, 0);
	fxNodeWalkPushReversed(binder->parser, &context, param, W_DISPATCH_B);
}

void fxWithNodeBind(void* it, void* param) 
{
	txWithNode* self = it;
	txBinder* binder = param;
	fxNodeWalkPush(binder->parser, self->expression, param, W_DISPATCH_B, 0);
	fxScopeBinding(self->scope, param);
	fxNodeWalkPush(binder->parser, self, param, WB_WITH, 0);
	fxNodeWalkPush(binder->parser, self->statement, param, W_DISPATCH_B, 0);
}

/* xs_no_recursion: hoist stage bodies. Each case runs only when its
   frame is on top of the walker stack; child visits push dispatch
   frames and return, and the frame resumes at its next stage. */
static void fxNodeHoistBody(txParser* parser)
{
	txNodeWalkFrame* f = parser->nodeWalkStack;
	switch (f->kind) {
	case WH_BLOCK: {
		txBlockNode* self = f->it;
		txHoister* hoister = f->param;
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_BODY: {
		txBodyNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 1) {
			/* XS_NR: statement dispatch finished; close scope on unwind */
			f->stage = 0;
			hoister->environmentNode = f->v0;
			fxScopeHoisted(self->scope, hoister);
			fxNodeWalkPop(parser);
			break;
		}
		hoister->environmentNode = f->v0;
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_CALL: {
		txCallNewNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->params, f->param, W_DISPATCH_H, 0);
			break;
		}
		fxNodeWalkPop(parser);
		break;
	}
	case WH_CATCH: {
		txCatchNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			self->statementScope = fxScopeNew(f->param, self, XS_TOKEN_BLOCK);
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->statement, f->param, W_DISPATCH_H, 0);
			break;
		}
		if (f->stage == 1) {
			txDeclareNode* node = self->statementScope->firstDeclareNode;
			fxScopeHoisted(self->statementScope, hoister);
			fxScopeHoisted(self->scope, hoister);
			while (node) {
			   if (fxScopeGetDeclareNode(self->scope, node->symbol))
				   fxReportParserError(hoister->parser, node->line, "duplicate variable %s", node->symbol->string);
			   node = node->nextDeclareNode;
			}
		}
		else
			fxScopeHoisted(self->statementScope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_CLASS: {
		txClassNode* self = f->it;
		txHoister* hoister = f->param;
		switch (f->stage) {
		case 0:
			fxClassNodeHoistItems(self, hoister);
			hoister->classNode = self;
			f->v1 = self->items->first;
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->constructor, f->param, W_DISPATCH_H, 0);
			break;
		case 1:
			if (f->v1) {
				txNode* item = f->v1;
				f->v1 = item->next;
				fxNodeWalkPush(hoister->parser, item, f->param, W_DISPATCH_H, 0);
				break;
			}
			if (self->constructorInit) {
				f->stage = 2;
				fxNodeWalkPush(hoister->parser, self->constructorInit, f->param, W_DISPATCH_H, 0);
				break;
			}
			/* fall through */
		case 2:
			if (f->stage == 2) {
				if (self->instanceInit) {
					f->stage = 3;
					fxNodeWalkPush(hoister->parser, self->instanceInit, f->param, W_DISPATCH_H, 0);
					break;
				}
			}
			else if (self->instanceInit) {
				f->stage = 3;
				fxNodeWalkPush(hoister->parser, self->instanceInit, f->param, W_DISPATCH_H, 0);
				break;
			}
			/* fall through */
		case 3:
			hoister->classNode = f->v0;
			fxScopeHoisted(self->scope, hoister);
			if (self->symbol)
				fxScopeHoisted(self->symbolScope, hoister);
			fxNodeWalkPop(parser);
			break;
		}
		break;
	}
	case WH_COALESCE: {
		txBinaryExpressionNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->right, f->param, W_DISPATCH_H, 0);
			break;
		}
		fxNodeWalkPop(parser);
		break;
	}
	case WH_DEFINE: {
		txDefineNode* self = f->it;
		((txFunctionNode*)(self->initializer))->symbol = self->symbol;
		fxNodeWalkPop(parser);
		break;
	}
	case WH_FOR: {
		txForNode* self = f->it;
		txHoister* hoister = f->param;
		switch (f->stage) {
		case 0:
			if (self->expression) {
				f->stage = 1;
				fxNodeWalkPush(hoister->parser, self->expression, f->param, W_DISPATCH_H, 0);
				break;
			}
			/* fall through */
		case 1:
			if (f->stage == 1) {
				if (self->iteration) {
					f->stage = 2;
					fxNodeWalkPush(hoister->parser, self->iteration, f->param, W_DISPATCH_H, 0);
					break;
				}
			}
			else if (self->iteration) {
				f->stage = 2;
				fxNodeWalkPush(hoister->parser, self->iteration, f->param, W_DISPATCH_H, 0);
				break;
			}
			/* fall through */
		case 2:
			f->stage = 3;
			fxNodeWalkPush(hoister->parser, self->statement, f->param, W_DISPATCH_H, 0);
			break;
		case 3:
			fxScopeHoisted(self->scope, hoister);
			fxNodeWalkPop(parser);
			break;
		}
		break;
	}
	case WH_FORINFOROF: {
		txForInForOfNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->expression, f->param, W_DISPATCH_H, 0);
		}
		else if (f->stage == 1) {
			f->stage = 2;
			fxNodeWalkPush(hoister->parser, self->statement, f->param, W_DISPATCH_H, 0);
		}
		else {
			fxScopeHoisted(self->scope, hoister);
			fxNodeWalkPop(parser);
		}
		break;
	}
	case WH_FUNCTION: {
		txFunctionNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			if ((self->flags & (mxArgumentsFlag | mxEvalFlag)) && !(self->flags & mxArrowFlag)) {
				txDeclareNode* declaration = fxDeclareNodeNew(hoister->parser, XS_TOKEN_VAR, hoister->parser->argumentsSymbol);
				fxScopeAddDeclareNode(hoister->functionScope, declaration);
			}
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->body, f->param, W_DISPATCH_H, 0);
			break;
		}
		fxScopeHoisted(self->scope, hoister);
		hoister->bodyScope = f->v1;
		hoister->functionScope = f->v0;
		fxNodeWalkPop(parser);
		break;
	}
	case WH_MODULE: {
		txModuleNode* self = f->it;
		txHoister* hoister = f->param;
		hoister->environmentNode = C_NULL;
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_PROGRAM: {
		txProgramNode* self = f->it;
		txHoister* hoister = f->param;
		hoister->environmentNode = C_NULL;
		self->variableCount = hoister->functionScope->declareNodeCount;
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_SWITCH: {
		txSwitchNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			self->scope = fxScopeNew(f->param, self, XS_TOKEN_BLOCK);
			f->v1 = self->items->first;
			f->stage = 1;
			if (f->v1) {
				txNode* item = f->v1;
				f->v1 = item->next;
				fxNodeWalkPush(hoister->parser, item, f->param, W_DISPATCH_H, 0);
				break;
			}
		}
		else {
			if (f->v1) {
				txNode* item = f->v1;
				f->v1 = item->next;
				fxNodeWalkPush(hoister->parser, item, f->param, W_DISPATCH_H, 0);
				break;
			}
		}
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	case WH_WITH: {
		txWithNode* self = f->it;
		txHoister* hoister = f->param;
		if (f->stage == 0) {
			self->scope = fxScopeNew(f->param, self, XS_TOKEN_WITH);
			fxScopeEval(hoister->scope);
			f->stage = 1;
			fxNodeWalkPush(hoister->parser, self->statement, f->param, W_DISPATCH_H, 0);
			break;
		}
		fxScopeHoisted(self->scope, hoister);
		fxNodeWalkPop(parser);
		break;
	}
	}
}

/* xs_no_recursion: bind stage bodies */
static void fxNodeBindBody(txParser* parser)
{
	txNodeWalkFrame* f = parser->nodeWalkStack;
	switch (f->kind) {
	case WB_ARRAY: {
		txArrayNode* self = f->it;
		txBinder* binder = f->param;
		if (self->flags & mxSpreadFlag)
			fxBinderPopVariables(binder, 2);
		fxBinderPopVariables(binder, 1);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_ARRAY_BINDING: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 6);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_ASSIGN: {
		txAssignNode* self = f->it;
		txBinder* binder = f->param;
		if (f->stage == 0) {
			txToken token = f->i0;
			if ((token == XS_TOKEN_ACCESS) || (token == XS_TOKEN_ARG) || (token == XS_TOKEN_CONST) || (token == XS_TOKEN_LET) || (token == XS_TOKEN_USING) || (token == XS_TOKEN_VAR))
				fxFunctionNodeRename(self->value, ((txAccessNode*)self->reference)->symbol);
			f->stage = 1;
			fxNodeWalkPush(binder->parser, self->value, f->param, W_DISPATCH_B, 0);
			break;
		}
		fxNodeWalkPop(parser);
		break;
	}
	case WB_BINDING: {
		txBindingNode* self = f->it;
		txBinder* binder = f->param;
		if (f->stage == 0) {
			txToken token = f->i0;
			if ((token == XS_TOKEN_ACCESS) || (token == XS_TOKEN_ARG) || (token == XS_TOKEN_CONST) || (token == XS_TOKEN_LET) || (token == XS_TOKEN_USING) || (token == XS_TOKEN_VAR))
				fxFunctionNodeRename(self->initializer, ((txAccessNode*)self->target)->symbol);
			f->stage = 1;
			fxNodeWalkPush(binder->parser, self->initializer, f->param, W_DISPATCH_B, 0);
			break;
		}
		fxNodeWalkPop(parser);
		break;
	}
	case WB_BLOCK: {
		txBlockNode* self = f->it;
		txBinder* binder = f->param;
		if (self->scope->disposableNodeCount)
			fxBinderPopVariables(binder, 2);
		fxScopeBound(self->scope, binder);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_CATCH: {
		txCatchNode* self = f->it;
		txBinder* binder = f->param;
		if (f->stage == 0) {
			fxScopeBinding(self->statementScope, binder);
			fxScopeBindDefineNodes(self->statementScope, binder);
			if (self->statementScope->disposableNodeCount)
				fxBinderPushVariables(binder, 2);
			f->stage = 1;
			fxNodeWalkPush(binder->parser, self->statement, f->param, W_DISPATCH_B, 0);
			break;
		}
		if (self->statementScope->disposableNodeCount)
			fxBinderPushVariables(binder, 2);
		fxScopeBound(self->statementScope, binder);
		if (f->i0)
			fxScopeBound(self->scope, binder);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_CLASS: {
		txClassNode* self = f->it;
		txBinder* binder = f->param;
		switch (f->stage) {
		case 0:
			fxScopeBinding(self->scope, binder);
			binder->classNode = self;
			f->v1 = self->items->first;
			f->stage = 1;
			fxNodeWalkPush(binder->parser, self->constructor, f->param, W_DISPATCH_B, 0);
			break;
		case 1:
			if (f->v1) {
				txNode* item = f->v1;
				f->v1 = item->next;
				fxNodeWalkPush(binder->parser, item, f->param, W_DISPATCH_B, 0);
				break;
			}
			if (self->constructorInit) {
				f->stage = 2;
				fxNodeWalkPush(binder->parser, self->constructorInit, f->param, W_DISPATCH_B, 0);
				break;
			}
			/* fall through */
		case 2:
			if (f->stage == 2) {
				if (self->instanceInit) {
					f->stage = 3;
					fxNodeWalkPush(binder->parser, self->instanceInit, f->param, W_DISPATCH_B, 0);
					break;
				}
			}
			else if (self->instanceInit) {
				f->stage = 3;
				fxNodeWalkPush(binder->parser, self->instanceInit, f->param, W_DISPATCH_B, 0);
				break;
			}
			/* fall through */
		case 3:
			binder->classNode = f->v0;
			fxScopeBound(self->scope, binder);
			if (self->symbol)
				fxScopeBound(self->symbolScope, binder);
			fxBinderPopVariables(binder, 2);
			fxNodeWalkPop(parser);
			break;
		}
		break;
	}
	case WB_DELEGATE: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 5);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_FOR: {
		txForNode* self = f->it;
		txBinder* binder = f->param;
		if (self->scope->disposableNodeCount)
			fxBinderPopVariables(binder, 2);
		fxScopeBound(self->scope, binder);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_FORINFOROF: {
		txForInForOfNode* self = f->it;
		txBinder* binder = f->param;
		fxScopeBound(self->scope, binder);
		fxBinderPopVariables(binder, 6);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_FUNCTION: {
		txFunctionNode* self = f->it;
		txBinder* binder = f->param;
		if (f->stage == 0) {
			if (self->flags & mxBaseFlag) {
				if (binder->classNode->instanceInitAccess)
					fxScopeLookup(binder->scope, binder->classNode->instanceInitAccess, 0);
			}
			fxScopeBindDefineNodes(self->scope, binder);
			f->stage = 1;
			fxNodeWalkPush(binder->parser, self->body, f->param, W_DISPATCH_B, 0);
			break;
		}
		fxScopeBound(self->scope, binder);
		self->scopeCount = binder->scopeMaximum;
		binder->scopeMaximum = f->i1;
		binder->scopeLevel = f->i0;
		fxNodeWalkPop(parser);
		break;
	}
	case WB_MODULE: {
		txModuleNode* self = f->it;
		txBinder* binder = f->param;
		if (self->scope->disposableNodeCount)
			fxBinderPopVariables(binder, 2);
		fxScopeBound(self->scope, binder);
		self->scopeCount = binder->scopeMaximum;
		fxNodeWalkPop(parser);
		break;
	}
	case WB_OBJECT: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 1);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_OBJECT_BINDING: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 2);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_PARAMS: {
		txParamsNode* self = f->it;
		txBinder* binder = f->param;
		if (self->flags & mxSpreadFlag)
			fxBinderPopVariables(binder, 1);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_PARAMS_BINDING:
		fxNodeWalkPop(parser);
		break;
	case WB_POSTFIX: {
		txBinder* binder = f->param;
		fxBinderPushVariables(binder, 1);
		fxBinderPopVariables(binder, 1);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_PROGRAM: {
		txProgramNode* self = f->it;
		txBinder* binder = f->param;
		fxScopeBound(self->scope, binder);
		self->scopeCount = binder->scopeMaximum;
		fxNodeWalkPop(parser);
		break;
	}
	case WB_SPREAD: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 1);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_SUPER: {
		txSuperNode* self = f->it;
		txBinder* binder = f->param;
		if (binder->classNode->instanceInitAccess) {
			self->instanceInitAccess = fxAccessNodeNew(binder->parser, XS_TOKEN_ACCESS, binder->classNode->instanceInitAccess->symbol);
			fxScopeLookup(binder->scope, self->instanceInitAccess, 0);
		}
		fxNodeWalkPop(parser);
		break;
	}
	case WB_SWITCH: {
		txSwitchNode* self = f->it;
		txBinder* binder = f->param;
		if (self->scope->disposableNodeCount)
			fxBinderPopVariables(binder, 2);
		fxScopeBound(self->scope, binder);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_TEMPLATE: {
		txTemplateNode* self = f->it;
		txBinder* binder = f->param;
		if (self->reference)
			fxBinderPopVariables(binder, 2);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_TRY: {
		txBinder* binder = f->param;
		fxBinderPopVariables(binder, 3);
		fxNodeWalkPop(parser);
		break;
	}
	case WB_WITH: {
		txWithNode* self = f->it;
		txBinder* binder = f->param;
		fxScopeBound(self->scope, binder);
		fxNodeWalkPop(parser);
		break;
	}
	}
}
