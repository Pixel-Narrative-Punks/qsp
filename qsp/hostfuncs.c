/* Copyright (C) 2001-2025 Val Argunov (byte AT qsp DOT org) */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "hostfuncs.h"
#include "callbacks.h"
#include "common.h"
#include "errors.h"
#include "locations.h"
#include "memory.h"
#include "statements.h"
#include "text.h"
#include "tuples.h"
#include "variant.h"

typedef struct
{
    QSP_CHAR *Name;
    QSP_CHAR *Profile;
    QSP_BOOL IsStatement;
    int MinArgsCount;
    int MaxArgsCount;
    int ResType;
    int Version[QSP_HOSTVERSIONPARTS]; /* the version of the profile that introduced the function */
} QSPStdHostFunc;

typedef struct
{
    QSP_CHAR *Name;
    int Version[QSP_HOSTVERSIONPARTS]; /* the latest version known to the engine */
} QSPStdHostProfile;

/* The standard functions & profiles, the only names allowed in the namespace "qsp." */
static QSPStdHostFunc qspStdHostFuncs[] =
{
    { QSP_FMT("QSP.DB.GET"), QSP_FMT("QSP.DB"), QSP_FALSE, 1, 1, QSP_TYPE_UNDEF, { 1, 0, 0 } },
    { QSP_FMT("QSP.DB.SET"), QSP_FMT("QSP.DB"), QSP_TRUE, 2, 2, QSP_TYPE_UNDEF, { 1, 0, 0 } },
    { QSP_FMT("QSP.DB.DEL"), QSP_FMT("QSP.DB"), QSP_TRUE, 1, 1, QSP_TYPE_UNDEF, { 1, 0, 0 } }
};

static QSPStdHostProfile qspStdHostProfiles[] =
{
    { QSP_FMT("QSP.DB"), { 1, 0, 0 } }
};

QSPHostFunc *qspHostFuncs = 0;
int qspHostFuncsCount = 0;
QSP_BOOL qspIsHostRegistryLocked = QSP_FALSE;
QSP_BOOL qspIsInHostCall = QSP_FALSE;

static int *qspHostFuncsOrder = 0; /* indices of the functions sorted by name */
static QSPHostProfile *qspHostProfiles = 0;
static int qspHostProfilesCount = 0;

INLINE QSP_BOOL qspCheckHostName(QSPString name, QSP_BOOL isDotRequired);
INLINE QSP_BOOL qspIsInStdNamespace(QSPString name);
INLINE QSPStdHostFunc *qspGetStdHostFunc(QSPString name);
INLINE QSPStdHostProfile *qspGetStdHostProfile(QSPString name);
INLINE QSPHostProfile *qspGetHostProfile(QSPString name);
INLINE QSP_BOOL qspParseHostVersion(QSPString version, int *parts);
INLINE int qspCompareHostVersions(int *first, int *second);
INLINE int qspHostFuncsOrderCompare(const void *index1, const void *index2);
INLINE int qspHostFuncNameCompare(const void *name, const void *index);
INLINE QSPString qspGetUpperName(QSPString name);
INLINE void qspNormalizeHostValues(QSPVariant *values, int count);

void qspInitHostFuncs(void)
{
    qspHostFuncs = 0;
    qspHostFuncsCount = 0;
    qspHostFuncsOrder = 0;
    qspHostProfiles = 0;
    qspHostProfilesCount = 0;
    qspIsHostRegistryLocked = QSP_FALSE;
    qspIsInHostCall = QSP_FALSE;
}

void qspTerminateHostFuncs(void)
{
    int i;
    qspReleaseHostFunctions();
    for (i = 0; i < qspHostFuncsCount; ++i)
    {
        qspFreeString(&qspHostFuncs[i].Name);
        qspFreeString(&qspHostFuncs[i].OrigName);
    }
    for (i = 0; i < qspHostProfilesCount; ++i)
    {
        qspFreeString(&qspHostProfiles[i].Name);
        qspFreeString(&qspHostProfiles[i].Version);
    }
    if (qspHostFuncs) free(qspHostFuncs);
    if (qspHostFuncsOrder) free(qspHostFuncsOrder);
    if (qspHostProfiles) free(qspHostProfiles);
    qspInitHostFuncs();
}

INLINE QSP_BOOL qspCheckHostName(QSPString name, QSP_BOOL isDotRequired)
{
    /* A valid identifier: no delimiters, no digit or type prefix at the start; the parts between dots aren't empty */
    QSP_BOOL hasDot = QSP_FALSE;
    QSP_CHAR *pos = name.Str, *partStart = name.Str;
    if (qspIsEmpty(name) || qspIsInClass(*pos, QSP_CHAR_DIGIT | QSP_CHAR_TYPEPREFIX)) return QSP_FALSE;
    for (; pos < name.End; ++pos)
    {
        if (qspIsInClass(*pos, QSP_CHAR_DELIM)) return QSP_FALSE;
        if (*pos == QSP_HOSTNAMESDELIM_CHAR)
        {
            if (pos == partStart) return QSP_FALSE;
            hasDot = QSP_TRUE;
            partStart = pos + 1;
        }
    }
    if (partStart == name.End) return QSP_FALSE;
    return (hasDot || !isDotRequired);
}

QSP_BOOL qspIsHostFuncName(QSPString name)
{
    return qspCheckHostName(name, QSP_TRUE);
}

INLINE QSP_BOOL qspIsInStdNamespace(QSPString name)
{
    /* The name is in upper case */
    QSPString ns = QSP_STATIC_STR(QSP_HOSTSTDNAMESPACE);
    int nsLen = qspStrLen(ns);
    if (qspStrLen(name) < nsLen || qspStrsPartCompare(name, ns)) return QSP_FALSE;
    return (qspStrLen(name) == nsLen || name.Str[nsLen] == QSP_HOSTNAMESDELIM_CHAR);
}

INLINE QSPStdHostFunc *qspGetStdHostFunc(QSPString name)
{
    int i;
    for (i = 0; i < (int)(sizeof(qspStdHostFuncs) / sizeof(qspStdHostFuncs[0])); ++i)
    {
        if (!qspStrsCompare(name, qspStringFromC(qspStdHostFuncs[i].Name)))
            return qspStdHostFuncs + i;
    }
    return 0;
}

INLINE QSPStdHostProfile *qspGetStdHostProfile(QSPString name)
{
    int i;
    for (i = 0; i < (int)(sizeof(qspStdHostProfiles) / sizeof(qspStdHostProfiles[0])); ++i)
    {
        if (!qspStrsCompare(name, qspStringFromC(qspStdHostProfiles[i].Name)))
            return qspStdHostProfiles + i;
    }
    return 0;
}

INLINE QSPHostProfile *qspGetHostProfile(QSPString name)
{
    int i;
    for (i = 0; i < qspHostProfilesCount; ++i)
    {
        if (!qspStrsCompare(name, qspHostProfiles[i].Name))
            return qspHostProfiles + i;
    }
    return 0;
}

INLINE QSP_BOOL qspParseHostVersion(QSPString version, int *parts)
{
    /* major.minor.patch: numbers without signs, spaces & leading zeros like in SemVer */
    int i, digit;
    QSP_CHAR *pos = version.Str, *end = version.End, *partStart;
    for (i = 0; i < QSP_HOSTVERSIONPARTS; ++i)
    {
        if (i)
        {
            if (pos >= end || *pos != QSP_HOSTNAMESDELIM_CHAR) return QSP_FALSE;
            ++pos;
        }
        partStart = pos;
        parts[i] = 0;
        while (pos < end && qspIsInClass(*pos, QSP_CHAR_DIGIT))
        {
            digit = *pos - QSP_FMT('0');
            if (parts[i] > (INT_MAX - digit) / 10) return QSP_FALSE;
            parts[i] = parts[i] * 10 + digit;
            ++pos;
        }
        if (pos == partStart) return QSP_FALSE;
        if (*partStart == QSP_FMT('0') && pos - partStart > 1) return QSP_FALSE;
    }
    return (pos == end);
}

INLINE int qspCompareHostVersions(int *first, int *second)
{
    int i;
    for (i = 0; i < QSP_HOSTVERSIONPARTS; ++i)
    {
        if (first[i] != second[i]) return (first[i] > second[i] ? 1 : -1);
    }
    return 0;
}

INLINE int qspHostFuncsOrderCompare(const void *index1, const void *index2)
{
    return qspStrsCompare(qspHostFuncs[*(int *)index1].Name, qspHostFuncs[*(int *)index2].Name);
}

INLINE int qspHostFuncNameCompare(const void *name, const void *index)
{
    return qspStrsCompare(*(QSPString *)name, qspHostFuncs[*(int *)index].Name);
}

INLINE QSPString qspGetUpperName(QSPString name)
{
    QSPString res = qspCopyToNewText(name);
    qspUpperStr(&res);
    return res;
}

int qspAddHostFunc(QSPString name, int minArgs, int maxArgs, int resType, QSP_BOOL isStatement)
{
    int index;
    QSPString upperName;
    QSPStdHostFunc *stdFunc;
    QSPHostFunc *func;
    if (qspIsHostRegistryLocked || qspIsInHostCall) return -1;
    if (minArgs < 0 || minArgs > maxArgs || maxArgs > QSP_MAXMATHOPARGS || maxArgs > QSP_MAXSTATARGS) return -1;
    if (isStatement) resType = QSP_TYPE_UNDEF;
    switch (resType)
    {
    case QSP_TYPE_UNDEF:
    case QSP_TYPE_TUPLE:
    case QSP_TYPE_NUM:
    case QSP_TYPE_STR:
        break;
    default:
        return -1;
    }
    if (!qspIsHostFuncName(name)) return -1;
    upperName = qspGetUpperName(name);
    if (qspHostFuncIndex(upperName) >= 0)
    {
        qspFreeString(&upperName);
        return -1;
    }
    if (qspIsInStdNamespace(upperName))
    {
        /* Only the standard functions with the standard signatures */
        stdFunc = qspGetStdHostFunc(upperName);
        if (!stdFunc || stdFunc->IsStatement != isStatement || stdFunc->MinArgsCount != minArgs ||
            stdFunc->MaxArgsCount != maxArgs || stdFunc->ResType != resType)
        {
            qspFreeString(&upperName);
            return -1;
        }
    }
    index = qspHostFuncsCount++;
    qspHostFuncs = (QSPHostFunc *)realloc(qspHostFuncs, qspHostFuncsCount * sizeof(QSPHostFunc));
    qspHostFuncsOrder = (int *)realloc(qspHostFuncsOrder, qspHostFuncsCount * sizeof(int));
    func = qspHostFuncs + index;
    func->Name = upperName;
    func->OrigName = qspCopyToNewText(name);
    func->MinArgsCount = (QSP_TINYINT)minArgs;
    func->MaxArgsCount = (QSP_TINYINT)maxArgs;
    func->ResType = (QSP_TINYINT)resType;
    func->IsStatement = isStatement;
    qspHostFuncsOrder[index] = index;
    qsort(qspHostFuncsOrder, qspHostFuncsCount, sizeof(int), qspHostFuncsOrderCompare);
    return index;
}

QSP_BOOL qspAddHostProfile(QSPString name, QSPString version)
{
    int i, parts[QSP_HOSTVERSIONPARTS];
    QSPString upperName;
    QSPHostProfile *profile;
    if (qspIsHostRegistryLocked || qspIsInHostCall) return QSP_FALSE;
    if (!qspCheckHostName(name, QSP_FALSE) || !qspParseHostVersion(version, parts)) return QSP_FALSE;
    upperName = qspGetUpperName(name);
    if (qspGetHostProfile(upperName))
    {
        qspFreeString(&upperName);
        return QSP_FALSE;
    }
    if (qspIsInStdNamespace(upperName))
    {
        /* A known version of a standard profile, all its functions have to be registered */
        QSPStdHostProfile *stdProfile = qspGetStdHostProfile(upperName);
        if (!stdProfile || parts[0] != stdProfile->Version[0] || qspCompareHostVersions(parts, stdProfile->Version) > 0)
        {
            qspFreeString(&upperName);
            return QSP_FALSE;
        }
        for (i = 0; i < (int)(sizeof(qspStdHostFuncs) / sizeof(qspStdHostFuncs[0])); ++i)
        {
            if (qspStrsCompare(upperName, qspStringFromC(qspStdHostFuncs[i].Profile))) continue;
            if (qspStdHostFuncs[i].Version[0] != parts[0] || qspCompareHostVersions(qspStdHostFuncs[i].Version, parts) > 0) continue;
            if (qspHostFuncIndex(qspStringFromC(qspStdHostFuncs[i].Name)) < 0)
            {
                qspFreeString(&upperName);
                return QSP_FALSE;
            }
        }
    }
    ++qspHostProfilesCount;
    qspHostProfiles = (QSPHostProfile *)realloc(qspHostProfiles, qspHostProfilesCount * sizeof(QSPHostProfile));
    profile = qspHostProfiles + qspHostProfilesCount - 1;
    profile->Name = upperName;
    profile->Version = qspCopyToNewText(version);
    for (i = 0; i < QSP_HOSTVERSIONPARTS; ++i)
        profile->VersionParts[i] = parts[i];
    return QSP_TRUE;
}

int qspHostFuncIndex(QSPString name)
{
    int *index;
    if (!qspHostFuncsCount) return -1;
    index = (int *)bsearch(&name, qspHostFuncsOrder, qspHostFuncsCount, sizeof(int), qspHostFuncNameCompare);
    return (index ? *index : -1);
}

int qspFindHostFunction(QSPString name)
{
    /* A function (not a statement), the type prefix has to match the type of the result like for the built-in functions */
    int index;
    QSP_TINYINT prefixType = QSP_TYPE_UNDEF;
    if (!qspIsEmpty(name) && qspIsInClass(*name.Str, QSP_CHAR_TYPEPREFIX))
    {
        switch (*name.Str)
        {
        case QSP_TUPLETYPE_CHAR: prefixType = QSP_TYPE_TUPLE; break;
        case QSP_NUMTYPE_CHAR: prefixType = QSP_TYPE_NUM; break;
        case QSP_STRTYPE_CHAR: prefixType = QSP_TYPE_STR; break;
        }
        name.Str += QSP_CHAR_LEN;
    }
    index = qspHostFuncIndex(name);
    if (index < 0 || qspHostFuncs[index].IsStatement) return -1;
    if (QSP_ISDEF(prefixType) && QSP_ISDEF(qspHostFuncs[index].ResType) && qspHostFuncs[index].ResType != prefixType)
        return -1;
    return index;
}

QSP_BOOL qspIsHostStatementCall(QSPString s)
{
    /* The text of the statement starts with a name */
    int index;
    QSPString name, rest = s;
    name = qspParseName(&rest);
    if (!qspIsHostFuncName(name)) return QSP_FALSE;
    index = qspHostFuncIndex(name);
    if (index >= 0) return qspHostFuncs[index].IsStatement;
    /* A missing function: assignments & expressions keep working like before */
    if (qspDelimPos(s, QSP_EQUAL_CHAR)) return QSP_FALSE;
    qspSkipSpaces(&rest);
    if (qspIsEmpty(rest) || *rest.Str == QSP_LSBRACK_CHAR || qspIsMathOperatorAt(rest)) return QSP_FALSE;
    return QSP_TRUE;
}

INLINE void qspNormalizeHostValues(QSPVariant *values, int count)
{
    /* The host gets the base types only: logical values are numbers, code & references are strings */
    while (--count >= 0)
    {
        values->Type = QSP_BASETYPE(values->Type);
        if (values->Type == QSP_TYPE_TUPLE)
            qspNormalizeHostValues(QSP_PTUPLE(values).Vals, QSP_PTUPLE(values).ValsCount);
        ++values;
    }
}

void qspSetHostResult(QSPVariant *res, QSPVariant *value)
{
    if (!res) return;
    qspFreeVariant(res);
    qspCopyToNewVariant(res, value);
    qspNormalizeHostValues(res, 1);
}

void qspExecHostFunction(int index, QSPVariant *args, QSP_TINYINT count, QSPVariant *res)
{
    QSP_BOOL isOk;
    QSPCallState state;
    QSPVariant result;
    QSPHostFunc *func = qspHostFuncs + index;
    qspNormalizeHostValues(args, count);
    qspInitVariant(&result, QSP_TYPE_UNDEF);
    qspPrepareCallback(&state, QSP_FALSE);
    qspIsInHostCall = QSP_TRUE;
    isOk = qspCallHostFunction(index, args, count, (func->IsStatement ? 0 : &result));
    qspIsInHostCall = QSP_FALSE;
    if (!qspFinalizeCallback(&state, QSP_FALSE))
    {
        qspFreeVariant(&result);
        return;
    }
    if (!isOk)
    {
        qspFreeVariant(&result);
        qspSetErrorWithDetails(QSP_ERR_HOSTFUNCFAILED, func->OrigName);
        return;
    }
    if (res)
    {
        /* The function guarantees the type of the result like the built-in functions do */
        if (QSP_ISDEF(func->ResType) && !qspConvertVariantTo(&result, func->ResType))
        {
            qspFreeVariant(&result);
            qspSetError(QSP_ERR_TYPEMISMATCH);
            return;
        }
        *res = result;
    }
    else
        qspFreeVariant(&result);
}

void qspSetHostFuncNotFoundError(QSPString name)
{
    qspSetErrorWithDetails(QSP_ERR_HOSTFUNCNOTFOUND, name);
}

void qspStatementHostCall(QSPCachedStat *stat)
{
    /* The first argument is the index of the statement or the name of a missing one */
    int i, oldLocationState;
    QSPVariant *args;
    QSP_TINYINT argsCount = stat->ArgsCount - 1;
    if (stat->Data.Args[0].Type == qspArgString)
    {
        qspSetHostFuncNotFoundError(stat->Data.Args[0].Data.Text);
        return;
    }
    oldLocationState = qspLocationState;
    args = (argsCount ? (QSPVariant *)qspAllocateMemory(argsCount * sizeof(QSPVariant)) : 0);
    for (i = 0; i < argsCount; ++i)
    {
        args[i] = qspCalculateArgValue(stat->Data.Args + i + 1);
        if (qspLocationState != oldLocationState)
        {
            qspFreeVariants(args, i);
            qspReleaseMemory(args);
            return;
        }
    }
    qspExecHostFunction((int)stat->Data.Args[0].Data.Number, args, argsCount, 0);
    if (args)
    {
        qspFreeVariants(args, argsCount);
        qspReleaseMemory(args);
    }
}

void qspFunctionHostFunc(QSPVariant *args, QSP_TINYINT QSP_UNUSED(count), QSPVariant *res)
{
    QSPString name = qspGetUpperName(QSP_STR(args[0]));
    QSP_PNUM(res) = QSP_TOBOOL(qspHostFuncIndex(name) >= 0);
    qspFreeString(&name);
}

void qspFunctionHostProfile(QSPVariant *args, QSP_TINYINT QSP_UNUSED(count), QSPVariant *res)
{
    QSPString name = qspGetUpperName(QSP_STR(args[0]));
    QSPHostProfile *profile = qspGetHostProfile(name);
    QSP_PSTR(res) = (profile ? qspCopyToNewText(profile->Version) : qspNullString);
    qspFreeString(&name);
}

void qspFunctionHostProfileMin(QSPVariant *args, QSP_TINYINT QSP_UNUSED(count), QSPVariant *res)
{
    int parts[QSP_HOSTVERSIONPARTS];
    QSPString name;
    QSPHostProfile *profile;
    if (!qspParseHostVersion(QSP_STR(args[1]), parts))
    {
        qspSetErrorWithDetails(QSP_ERR_INCORRECTVERSION, QSP_STR(args[1]));
        return;
    }
    name = qspGetUpperName(QSP_STR(args[0]));
    profile = qspGetHostProfile(name);
    QSP_PNUM(res) = QSP_TOBOOL(profile && profile->VersionParts[0] == parts[0] &&
        qspCompareHostVersions(profile->VersionParts, parts) >= 0);
    qspFreeString(&name);
}

QSP_BOOL qspExecHostEvent(QSPString locName, QSPVariant *args, int count)
{
    int i;
    QSPVariant eventArgs[QSP_MAXSTATARGS];
    if (qspIsInCallback || count < 0 || count > QSP_MAXSTATARGS || (count && !args)) return QSP_FALSE;
    for (i = 0; i < count; ++i)
        qspCopyToNewVariant(eventArgs + i, args + i);
    qspNormalizeHostValues(eventArgs, count);
    qspPrepareExecution(QSP_FALSE);
    qspExecLocByNameWithArgs(locName, eventArgs, (QSP_TINYINT)count, QSP_FALSE, 0);
    qspFreeVariants(eventArgs, count);
    return (QSP_BOOL)(qspErrorNum == 0);
}
