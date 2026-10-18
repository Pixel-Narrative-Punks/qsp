/* Copyright (C) 2001-2025 Val Argunov (byte AT qsp DOT org) */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "declarations.h"
#include "codetools.h"

#ifndef QSP_HOSTFUNCSDEFINES
    #define QSP_HOSTFUNCSDEFINES

    /* Names of the built-in functions that check the host */
    #define QSP_FUNC_HOSTFUNC QSP_FMT("HOSTFUNC")
    #define QSP_FUNC_HOSTPROFILE QSP_FMT("HOSTPROF")
    #define QSP_FUNC_HOSTPROFILEMIN QSP_FMT("HOSTPROFMIN")

    /* The namespace of the standard functions & profiles */
    #define QSP_HOSTSTDNAMESPACE QSP_FMT("QSP")
    #define QSP_HOSTNAMESDELIM QSP_FMT(".")
    QSP_DEFINE_SPECIAL_CHAR(QSP_HOSTNAMESDELIM, QSP_FMT('.'))

    #define QSP_HOSTVERSIONPARTS 3 /* major.minor.patch */

    typedef struct
    {
        QSPString Name; /* in upper case, the way the code refers to it */
        QSPString OrigName; /* as registered, for error messages & the host */
        QSP_TINYINT MinArgsCount;
        QSP_TINYINT MaxArgsCount;
        QSP_TINYINT ResType; /* QSP_TYPE_UNDEF for functions without a fixed type & for statements */
        QSP_BOOL IsStatement;
    } QSPHostFunc;

    typedef struct
    {
        QSPString Name; /* in upper case */
        QSPString Version; /* as registered */
        int VersionParts[QSP_HOSTVERSIONPARTS];
    } QSPHostProfile;

    extern QSPHostFunc *qspHostFuncs; /* in the order of registration, the index identifies the function */
    extern int qspHostFuncsCount;
    extern QSP_BOOL qspIsHostRegistryLocked; /* registration is closed once any code got loaded or executed */
    extern QSP_BOOL qspIsInHostCall; /* a host function is running, the API can only read the state */

    /* External functions */
    void qspInitHostFuncs(void);
    void qspTerminateHostFuncs(void);
    /* Registration, the bindings keep their handlers by the returned index */
    int qspAddHostFunc(QSPString name, int minArgs, int maxArgs, int resType, QSP_BOOL isStatement);
    QSP_BOOL qspAddHostProfile(QSPString name, QSPString version);
    void qspSetHostResult(QSPVariant *res, QSPVariant *value);
    /* Lookup, the names are in upper case */
    QSP_BOOL qspIsHostFuncName(QSPString name);
    int qspHostFuncIndex(QSPString name);
    int qspFindHostFunction(QSPString name);
    QSP_BOOL qspIsHostStatementCall(QSPString s);
    /* Execution */
    void qspExecHostFunction(int index, QSPVariant *args, QSP_TINYINT count, QSPVariant *res);
    void qspSetHostFuncNotFoundError(QSPString name);
    void qspStatementHostCall(QSPCachedStat *stat);
    void qspFunctionHostFunc(QSPVariant *args, QSP_TINYINT count, QSPVariant *res);
    void qspFunctionHostProfile(QSPVariant *args, QSP_TINYINT count, QSPVariant *res);
    void qspFunctionHostProfileMin(QSPVariant *args, QSP_TINYINT count, QSPVariant *res);
    QSP_BOOL qspExecHostEvent(QSPString locName, QSPVariant *args, int count);

#endif
