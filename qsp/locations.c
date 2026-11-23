/* Copyright (C) 2001-2025 Val Argunov (byte AT qsp DOT org) */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "locations.h"
#include "actions.h"
#include "callbacks.h"
#include "codetools.h"
#include "common.h"
#include "errors.h"
#include "names.h"
#include "statements.h"
#include "text.h"
#include "variables.h"

QSPLocation **qspLocs = 0;
int qspLocsCount = 0;
int qspLocsCapacity = 0;
int qspMainLocsCount = 0;

QSPLocation *qspCurLoc = 0;
int qspLocationState = 0;
int qspFullRefreshCount = 0;
int qspCurCallDepth = 0;

static QSPLocation **qspLocsSlots = 0; /* hash table by names, QSP_SLOTSCOUNT(qspLocsCapacity) slots */

INLINE QSPLocation **qspGetLocSlot(int nameIndex);
INLINE QSPLocation **qspGetEmptyLocSlot(int nameIndex);
INLINE void qspRebuildLocsSlots(void);
INLINE void qspResizeLocs(int newCapacity);
INLINE void qspAddBaseDescAndActions(QSPLocation *loc);
INLINE void qspExecLocation(QSPLocation *loc, QSP_BOOL toChangeDesc);

INLINE QSPLocation **qspGetLocSlot(int nameIndex)
{
    QSPLocation **slot;
    int mask = QSP_SLOTSMASK(qspLocsCapacity), ind = (int)(qspGetIndexHash(nameIndex) & mask);
    while (*(slot = qspLocsSlots + ind) && (*slot)->NameIndex != nameIndex)
        ind = (ind + 1) & mask;
    return slot;
}

INLINE QSPLocation **qspGetEmptyLocSlot(int nameIndex)
{
    int mask = QSP_SLOTSMASK(qspLocsCapacity), ind = (int)(qspGetIndexHash(nameIndex) & mask);
    while (qspLocsSlots[ind]) ind = (ind + 1) & mask;
    return qspLocsSlots + ind;
}

INLINE void qspRebuildLocsSlots(void)
{
    int i;
    QSPLocation **slot;
    memset(qspLocsSlots, 0, QSP_SLOTSCOUNT(qspLocsCapacity) * sizeof(QSPLocation *));
    for (i = 0; i < qspLocsCount; ++i)
    {
        slot = qspGetEmptyLocSlot(qspLocs[i]->NameIndex);
        *slot = qspLocs[i];
    }
}

INLINE void qspResizeLocs(int newCapacity)
{
    qspLocsCapacity = newCapacity;
    qspLocs = (QSPLocation **)realloc(qspLocs, newCapacity * sizeof(QSPLocation *));
    qspLocsSlots = (QSPLocation **)realloc(qspLocsSlots, QSP_SLOTSCOUNT(newCapacity) * sizeof(QSPLocation *));
    qspRebuildLocsSlots();
}

void qspInitWorld(void)
{
    qspLocsCount = 0;
    qspLocsCapacity = QSP_LOCSCAPACITY;
    qspMainLocsCount = 0;
    qspLocs = (QSPLocation **)malloc(qspLocsCapacity * sizeof(QSPLocation *));
    qspLocsSlots = (QSPLocation **)malloc(QSP_SLOTSCOUNT(qspLocsCapacity) * sizeof(QSPLocation *));
    memset(qspLocsSlots, 0, QSP_SLOTSCOUNT(qspLocsCapacity) * sizeof(QSPLocation *));
}

void qspTerminateWorld(void)
{
    qspTruncateWorld(0);
    free(qspLocs);
    free(qspLocsSlots);
}

void qspTruncateWorld(int locsCount)
{
    int i;
    if (locsCount < 0 || locsCount >= qspLocsCount) return;
    for (i = locsCount; i < qspLocsCount; ++i)
    {
        if (qspLocs[i] == qspCurLoc)
            qspUpdateLocation(&qspCurLoc, 0); /* the current location has to be valid */
        qspReleaseLocation(qspLocs[i]);
    }
    qspLocsCount = locsCount;
    if (locsCount < qspMainLocsCount)
        qspMainLocsCount = locsCount;
    qspRebuildLocsSlots(); /* drops the slots of the removed locations */
}

void qspRemoveIncFileLocations(int fileIndex)
{
    int i, locsCount;
    if (fileIndex < 0) return;
    if (qspCurLoc && qspCurLoc->FileIndex == fileIndex)
        qspUpdateLocation(&qspCurLoc, 0); /* the current location has to be valid */
    locsCount = qspMainLocsCount;
    for (i = qspMainLocsCount; i < qspLocsCount; ++i)
    {
        if (qspLocs[i]->FileIndex == fileIndex)
            qspReleaseLocation(qspLocs[i]);
        else
        {
            /* Re-insert location that doesn't belong to the specified file */
            qspLocs[locsCount++] = qspLocs[i];
        }
    }
    if (locsCount < qspLocsCount)
    {
        qspLocsCount = locsCount;
        qspRebuildLocsSlots(); /* drops the slots of the removed locations */
    }
}

void qspFreeLocation(QSPLocation *loc)
{
    qspFreeString(&loc->Name);
    qspFreeString(&loc->Desc);
    if (loc->OnVisitCode) qspReleaseCodeBlock(loc->OnVisitCode);
    if (loc->Actions)
    {
        int i;
        QSPLocAct *curAct;
        for (i = 0, curAct = loc->Actions; i < loc->ActionsCount; ++i, ++curAct)
        {
            qspFreeString(&curAct->Image);
            qspFreeString(&curAct->Desc);
            if (curAct->OnPressCode) qspReleaseCodeBlock(curAct->OnPressCode);
        }
        free(loc->Actions);
    }
}

QSPLocation *qspAddLocation(QSPString name)
{
    QSPLocation *loc, **slot;
    int nameIndex;
    name = qspDelSpc(name);
    nameIndex = qspGetNameIndex(name, QSP_TRUE);
    if (!nameIndex) return 0; /* empty names aren't allowed */
    slot = qspGetLocSlot(nameIndex);
    if (*slot) return 0;
    if (qspLocsCount >= qspLocsCapacity)
    {
        qspResizeLocs(qspLocsCapacity * 2);
        slot = qspGetEmptyLocSlot(nameIndex);
    }
    /* Create a new location */
    loc = (QSPLocation *)malloc(sizeof(QSPLocation));
    loc->Name = qspCopyToNewText(name);
    loc->NameIndex = nameIndex;
    loc->FileIndex = -1;
    loc->Desc = qspNullString;
    loc->OnVisitCode = 0;
    loc->Actions = 0;
    loc->ActionsCount = 0;
    loc->RefsCount = 1;
    qspLocs[qspLocsCount++] = loc;
    *slot = loc;
    return loc;
}

int qspGetLocNameIndex(QSPString name, QSP_BOOL toIntern)
{
    name = qspDelSpc(name);
    return qspGetNameIndex(name, toIntern);
}

QSPLocation *qspGetLoc(int nameIndex)
{
    if (nameIndex > 0) /* empty names aren't allowed */
        return *qspGetLocSlot(nameIndex);
    return 0;
}

QSPLocation *qspGetLocByName(QSPString name)
{
    return qspGetLoc(qspGetLocNameIndex(name, QSP_FALSE));
}

INLINE void qspAddBaseDescAndActions(QSPLocation *loc)
{
    QSPString text;
    QSPLocAct *curAct;
    int i, endLine, oldLocationState = qspLocationState;
    /* Update base description */
    if (!qspIsEmpty(loc->Desc))
    {
        text = qspFormatText(loc->Desc);
        if (qspLocationState != oldLocationState)
        {
            qspFreeString(&text);
            return;
        }
        if (qspAddBufText(&qspCurDesc, text))
            qspCurWindowsChangedState |= QSP_WIN_MAIN;
        qspFreeString(&text);
    }
    /* Update base actions */
    for (i = 0, curAct = loc->Actions; i < loc->ActionsCount; ++i, ++curAct)
    {
        if (qspIsEmpty(curAct->Desc)) break;
        qspRunActIndex = i;
        text = qspFormatText(curAct->Desc);
        if (qspLocationState != oldLocationState)
        {
            qspFreeString(&text);
            return;
        }
        endLine = (curAct->OnPressCode ? curAct->OnPressCode->LinesCount : 0);
        if (qspIsAnyString(curAct->Image))
            qspAddAction(text, curAct->Image, curAct->OnPressCode, 0, endLine);
        else
            qspAddAction(text, qspNullString, curAct->OnPressCode, 0, endLine);
        qspFreeString(&text);
        if (qspLocationState != oldLocationState) return;
    }
}

INLINE void qspExecLocation(QSPLocation *loc, QSP_BOOL toChangeDesc)
{
    QSPRunState oldState;
    int oldLocationState = qspLocationState;
    /* Switch the current state */
    qspSaveRunState(&oldState);
    qspUpdateLocation(&qspRunLoc, loc);
    if (toChangeDesc && qspCurDesc.Len > 0)
    {
        qspClearBufString(&qspCurDesc);
        qspCurWindowsChangedState |= QSP_WIN_MAIN;
    }
    if (qspIsDebug)
    {
        /* Trigger debugger only if we have base description or base actions */
        if (!qspIsEmpty(loc->Desc) || loc->ActionsCount)
        {
            qspCallDebug(qspNullString);
            if (qspLocationState != oldLocationState)
            {
                qspUpdateLocation(&qspRunLoc, 0);
                qspRestoreRunState(&oldState);
                return;
            }
        }
    }
    qspAddBaseDescAndActions(loc);
    if (qspLocationState != oldLocationState)
    {
        qspUpdateLocation(&qspRunLoc, 0);
        qspRestoreRunState(&oldState);
        return;
    }
    /* Execute the code */
    if (loc->OnVisitCode)
    {
        qspRunActIndex = -1;
        qspRunCode = loc->OnVisitCode;
        qspExecCode(loc->OnVisitCode, 0, loc->OnVisitCode->LinesCount, 0);
    }
    /* Restore the old state */
    qspUpdateLocation(&qspRunLoc, 0);
    qspRestoreRunState(&oldState);
}

void qspExecLocWithArgs(int nameIndex, QSPVariant *args, QSP_TINYINT argsCount, QSP_BOOL toMoveArgs, QSPVariant *res)
{
    int oldLocalScopeBase, oldLocationState;
    QSPLocation *loc = qspGetLoc(nameIndex);
    if (!loc)
    {
        qspSetError(QSP_ERR_LOCNOTFOUND);
        return;
    }
    if (qspCurCallDepth >= QSP_MAXCALLDEPTH)
    {
        qspSetError(QSP_ERR_CALLDEPTH);
        return;
    }
    oldLocalScopeBase = qspAllocateLocalScopeWithArgs(args, argsCount, toMoveArgs);

    oldLocationState = qspLocationState;
    ++qspCurCallDepth;
    qspExecLocation(loc, QSP_FALSE);
    --qspCurCallDepth;
    if (qspLocationState != oldLocationState) return;

    if (res && !qspApplyResult(res)) return;
    qspReleaseLocalScope(oldLocalScopeBase);
}

void qspExecLocByNameWithArgs(QSPString name, QSPVariant *args, QSP_TINYINT argsCount, QSP_BOOL toMoveArgs, QSPVariant *res)
{
    int nameIndex = qspGetLocNameIndex(name, QSP_FALSE);
    qspExecLocWithArgs(nameIndex, args, argsCount, toMoveArgs, res);
}

void qspExecLocByVarNameWithArgs(QSPString name, QSPVariant *args, QSP_TINYINT argsCount)
{
    QSPVarData *varData;
    QSPString locName;
    int oldLocalVarsBase, ind, oldLocationState;
    /* Hide local variables */
    oldLocalVarsBase = qspHideLocalVars();
    /* We execute all locations specified in the array */
    oldLocationState = qspLocationState;
    ind = 0;
    while (1)
    {
        /* The variable might be updated during the previous code execution */
        if (!((varData = qspVarDataByName(name, QSP_REF_GET, 0))))
        {
            qspClearHiddenLocalVars(oldLocalVarsBase);
            return;
        }
        if (ind >= varData->ValsCount) break;
        if (!QSP_ISSTR(varData->Values[ind].Type)) break;
        locName = QSP_STR(varData->Values[ind]);
        if (!qspIsAnyString(locName)) break;
        qspExecLocByNameWithArgs(locName, args, argsCount, QSP_FALSE, 0);
        if (qspLocationState != oldLocationState)
        {
            qspClearHiddenLocalVars(oldLocalVarsBase);
            return;
        }
        ++ind;
    }
    qspRestoreHiddenLocalVars(oldLocalVarsBase);
}

void qspNavigateToLocation(QSPLocation *loc, QSP_BOOL toChangeDesc, QSPVariant *args, QSP_TINYINT argsCount)
{
    int oldLocalScopeBase, oldLocationState;
    if (qspCurCallDepth >= QSP_MAXCALLDEPTH)
    {
        qspSetError(QSP_ERR_CALLDEPTH);
        return;
    }
    qspUpdateLocation(&qspCurLoc, loc);
    /* Restore global variables */
    qspClearLocalVars();
    /* We assign global ARGS here */
    qspSetArgs(args, argsCount);

    qspClearAllActions(QSP_FALSE);
    ++qspLocationState;
    if (toChangeDesc) ++qspFullRefreshCount;
    oldLocalScopeBase = qspAllocateLocalScope();

    oldLocationState = qspLocationState;
    ++qspCurCallDepth;
    qspExecLocation(loc, toChangeDesc);
    --qspCurCallDepth;
    if (qspLocationState != oldLocationState) return;

    qspReleaseLocalScope(oldLocalScopeBase);
    qspExecLocByVarNameWithArgs(QSP_STATIC_STR(QSP_LOC_NEWLOC), args, argsCount);
}
