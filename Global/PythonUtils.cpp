/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#undef Py_LIMITED_API  // Needed for Py_NoUserSiteDirectory, PyBytes_AS_STRING
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PythonUtils.h"

#include <cstdlib>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma warning (disable : 4996)
#else
#include <dirent.h>
#endif

#include "../Global/FStreamsSupport.h"
#include "../Global/ProcInfo.h"
#include "../Global/StrUtils.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

#if PY_VERSION_HEX >= 0x030D0000
// Python 3.13+: store pythonHome from setupPythonEnv() for use in PyConfig-based initialization
static std::wstring s_pythonHomeW;
#endif

static bool fileExists(const std::string& path)
{
    FStreamsSupport::ifstream ifile;
    FStreamsSupport::open(&ifile, path);
    return ifile.good();
}

static bool dirExists(const std::string& path)
{
    // https://stackoverflow.com/q/18100097
    struct stat info;

    if(stat(path.c_str(), &info ) != 0) {
        return false;
    } else if(info.st_mode & S_IFDIR) {
        return true;
    }
    return false;
}

void setupPythonEnv(const std::string& binPath)
{
    //Disable user sites as they could conflict with Natron bundled packages.
    //If this is set, Python won’t add the user site-packages directory to sys.path.
    //See https://www.python.org/dev/peps/pep-0370/
    ProcInfo::putenv_wrapper("PYTHONNOUSERSITE", "1");
#if PY_VERSION_HEX < 0x030D0000
    ++Py_NoUserSiteDirectory;
#endif

    //
    // set up paths, clear those that don't exist or are not valid
    //
#ifdef __NATRON_WIN32__
    static std::string pythonHome = binPath + "\\.."; // must use static storage
    static const std::wstring pythonHomeW = StrUtils::utf8_to_utf16(pythonHome);
    std::string pyPathZip = pythonHome + "\\lib\\python" NATRON_PY_VERSION_STRING_NO_DOT ".zip";
    std::string pyPath = pythonHome +  "\\lib\\python" NATRON_PY_VERSION_STRING;
    std::string pyPathDynLoad = pyPath + "\\lib-dynload";
    std::string pyPathSitePackages = pyPath + "\\site-packages";
    std::string pluginPath = binPath + "\\..\\Plugins";
#else
#  if defined(__NATRON_LINUX__)
    static std::string pythonHome = binPath + "/.."; // must use static storage
#  elif defined(__NATRON_OSX__)
    static std::string pythonHome = binPath+ "/../Frameworks/Python.framework/Versions/" NATRON_PY_VERSION_STRING; // must use static storage
#  else
#    error "unsupported platform"
#  endif
    static const std::wstring pythonHomeW = StrUtils::utf8_to_utf16(pythonHome);
    std::string pyPathZip = pythonHome + "/lib/python" NATRON_PY_VERSION_STRING_NO_DOT ".zip";
    std::string pyPath = pythonHome + "/lib/python" NATRON_PY_VERSION_STRING;
    std::string pyPathDynLoad = pyPath + "/lib-dynload";
    std::string pyPathSitePackages = pyPath + "/site-packages";
    std::string pluginPath = binPath + "/../Plugins";
#endif
    if ( !fileExists( StrUtils::fromNativeSeparators(pyPathZip) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathZip.c_str() );
#     endif
        pyPathZip.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPath) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPath.c_str() );
#     endif
        pyPath.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPathDynLoad) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathDynLoad.c_str() );
#     endif
        pyPathDynLoad.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPathSitePackages) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathSitePackages.c_str() );
#     endif
        pyPathSitePackages.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pluginPath) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pluginPath.c_str() );
#     endif
        pluginPath.clear();
    }
    // PYTHONHOME is really useful if there's a python inside it
    if ( pyPathZip.empty() && pyPath.empty() ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "dir \"%s\" does not exist or does not contain lib/python*, not setting PYTHONHOME\n", pythonHome.c_str() );
#     endif
        pythonHome.clear();
    }

    /////////////////////////////////////////
    // Py_SetPythonHome
    /////////////////////////////////////////
    //
    // Must be done before Py_Initialize (see doc of Py_Initialize)
    //
    // The argument should point to a zero-terminated character string in static storage whose contents will not change for the duration of the program’s execution

    if ( !pythonHome.empty() ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "Py_SetPythonHome(\"%s\")\n", pythonHome.c_str() );
#     endif
#if PY_VERSION_HEX >= 0x030D0000
        // Python 3.13+: Py_SetPythonHome is removed; store for PyConfig-based init
        s_pythonHomeW = pythonHomeW;
#else
        Py_SetPythonHome( const_cast<wchar_t*>( pythonHomeW.c_str() ) );
#endif
    }


    /////////////////////////////////////////
    // PYTHONPATH and Py_SetPath
    /////////////////////////////////////////
    //
    // note: to check the python path of a python install, execute:
    // python -c 'import sys,pprint; pprint.pprint( sys.path )'
    //
    // to build the python27.zip, cd to lib/python2.7, and generate the pyo and the zip file using:
    //
    //  python -O -m compileall .
    //  zip -r ../python27.zip *.py* bsddb compiler ctypes curses distutils email encodings hotshot idlelib importlib json logging multiprocessing pydoc_data sqlite3 unittest wsgiref xml
    //
    std::string pythonPath = ProcInfo::getenv_wrapper("PYTHONPATH");
    //Add the Python distribution of Natron to the Python path

    std::vector<std::string> toPrepend;
    if ( !pyPathZip.empty() ) {
        toPrepend.push_back(pyPathZip);
    }
    if ( !pyPath.empty() ) {
        toPrepend.push_back(pyPath);
    }
    if ( !pyPathDynLoad.empty() ) {
        toPrepend.push_back(pyPathDynLoad);
    }
    if ( !pyPathSitePackages.empty() ) {
        toPrepend.push_back(pyPathSitePackages);
    }
    if ( !pluginPath.empty() ) {
        toPrepend.push_back(pluginPath);
    }

#if defined(__NATRON_OSX__) && defined DEBUG
    // in debug mode, also prepend the local PySide directory
    // homebrew's pyside directory
    toPrepend.push_back("/usr/local/Cellar/pyside@1.2/1.2.2_2/lib/python" NATRON_PY_VERSION_STRING "/site-packages");
    // macport's pyside directory
    toPrepend.push_back("/opt/local/Library/Frameworks/Python.framework/Versions/" NATRON_PY_VERSION_STRING "/lib/python" NATRON_PY_VERSION_STRING "/site-packages");
#endif

    if ( toPrepend.empty() ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf("PYTHONPATH not modified\n");
#     endif
    } else {
#     ifdef __NATRON_WIN32__
        const char pathSep = ';';
#     else
        const char pathSep = ':';
#     endif
        std::string toPrependStr = StrUtils::join(toPrepend, pathSep);
        if (pythonPath.empty()) {
            pythonPath = toPrependStr;
        } else {
            pythonPath = toPrependStr + pathSep + pythonPath;
        }
        // Py_SetPath() sets the whole path, but setting PYTHONPATH still keeps the system's python path
        ProcInfo::putenv_wrapper( "PYTHONPATH", pythonPath.c_str() );
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "PYTHONPATH set to %s\n", pythonPath.c_str() );
#     endif
    }

} // setupPythonEnv

PyObject* initializePython3(const std::vector<wchar_t*>& commandLineArgsWide)
{
#if PY_VERSION_HEX >= 0x030D0000
    /////////////////////////////////////////
    // Python 3.13+ initialization using PyConfig API
    /////////////////////////////////////////
    //
    // The legacy APIs (Py_SetProgramName, Py_SetPythonHome, Py_Initialize,
    // PySys_SetArgv, PyEval_InitThreads) were removed in Python 3.13+.
    // Use the structured PyConfig API instead.

    PyStatus status;
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    config.user_site_directory = 0;

    // Set program name
    status = PyConfig_SetString(&config, &config.program_name, commandLineArgsWide[0]);
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        Py_ExitStatusException(status);
        return nullptr;
    }

    // Set home directory (stored by setupPythonEnv)
    if (!s_pythonHomeW.empty()) {
        status = PyConfig_SetString(&config, &config.home, s_pythonHomeW.c_str());
        if (PyStatus_Exception(status)) {
            PyConfig_Clear(&config);
            Py_ExitStatusException(status);
            return nullptr;
        }
    }

    // Set argv
    status = PyConfig_SetArgv(&config, (int)commandLineArgsWide.size(),
                               const_cast<wchar_t* const*>(&commandLineArgsWide[0]));
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        Py_ExitStatusException(status);
        return nullptr;
    }

#if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
    printf("Py_InitializeFromConfig()\n");
#endif
    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        Py_ExitStatusException(status);
        return nullptr;
    }
    PyConfig_Clear(&config);

    // Set sys.prefix and sys.exec_prefix from home
    if (!s_pythonHomeW.empty()) {
        PyObject *prefix = PyUnicode_FromWideChar(s_pythonHomeW.c_str(), -1);
        PySys_SetObject(const_cast<char*>("prefix"), prefix);
        Py_XDECREF(prefix);
        PyObject *exec_prefix = PyUnicode_FromWideChar(s_pythonHomeW.c_str(), -1);
        PySys_SetObject(const_cast<char*>("exec_prefix"), exec_prefix);
        Py_XDECREF(exec_prefix);
    }

    PyObject* mainModule = PyImport_ImportModule("__main__");

#if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
    /// print info about python lib (using sys module queries for 3.13+)
    {
        printf( "PATH is %s\n", Py_GETENV("PATH") );
        printf( "PYTHONPATH is %s\n", Py_GETENV("PYTHONPATH") );
        printf( "PYTHONHOME is %s\n", Py_GETENV("PYTHONHOME") );

        // Global flags are removed in 3.13+; query sys.flags instead
        PyObject* sysModule = PyImport_ImportModule("sys");
        if (sysModule) {
            PyObject* flags = PyObject_GetAttrString(sysModule, "flags");
            if (flags) {
                PySys_FormatStdout("  sys.flags = %A\n", flags);
                Py_DECREF(flags);
            }
            Py_DECREF(sysModule);
        }

        printf( "Py_GetProgramName is %ls\n", Py_GetProgramName() );
        printf( "Py_GetPrefix is %ls\n", Py_GetPrefix() );
        printf( "Py_GetExecPrefix is %ls\n", Py_GetExecPrefix() );
        printf( "Py_GetProgramFullPath is %ls\n", Py_GetProgramFullPath() );
        printf( "Py_GetPath is %ls\n", Py_GetPath() );

#define DUMP_SYS(NAME) \
            do { \
                obj = PySys_GetObject(#NAME); \
                PySys_FormatStderr("  sys.%s = ", #NAME); \
                if (obj != NULL) { \
                    PySys_FormatStdout("%A", obj); \
                } \
                else { \
                    PySys_WriteStdout("(not set)"); \
                } \
                PySys_FormatStdout("\n"); \
            } while (0)

        PyObject *obj;
        DUMP_SYS(version);
        DUMP_SYS(_base_executable);
        DUMP_SYS(base_prefix);
        DUMP_SYS(base_exec_prefix);
        DUMP_SYS(platlibdir);
        DUMP_SYS(executable);
        DUMP_SYS(prefix);
        DUMP_SYS(exec_prefix);
#undef DUMP_SYS

        PyObject *sys_path = PySys_GetObject("path");
        if (sys_path != NULL && PyList_Check(sys_path)) {
            PySys_WriteStdout("  sys.path = [\n");
            Py_ssize_t len = PyList_GET_SIZE(sys_path);
            for (Py_ssize_t i=0; i < len; i++) {
                PyObject *path = PyList_GET_ITEM(sys_path, i);
                PySys_FormatStdout("    %A,\n", path);
            }
            PySys_WriteStdout("  ]\n");
        }

        PyObject* dict = PyModule_GetDict(mainModule);
        PyErr_Clear();

        // distutils was removed in Python 3.12; use sysconfig instead
        std::string script("import sysconfig; print('Python library is in ' + sysconfig.get_path('purelib'))");
        PyObject* v = PyRun_String(script.c_str(), Py_file_input, dict, 0);
        if (v) {
            Py_DECREF(v);
        }
    }
#endif // DEBUG

    // Release the GIL for multi-threaded use
    PyThreadState *_save = PyEval_SaveThread();

    return mainModule;

#else // PY_VERSION_HEX < 0x030D0000
    /////////////////////////////////////////
    // Legacy Python initialization (Python < 3.13)
    /////////////////////////////////////////

    Py_SetProgramName(commandLineArgsWide[0]);

#if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
    printf("Py_Initialize()\n");
#endif
    Py_Initialize();

    // Py_SetPath clears sys.prefix and sys.exec_prefix
    // https://github.com/NatronGitHub/Natron/issues/696
    PyObject *prefix = PyUnicode_FromWideChar(Py_GetPythonHome(), -1);
    PySys_SetObject(const_cast<char*>("prefix"), prefix);
    Py_XDECREF(prefix);
    PyObject *exec_prefix = PyUnicode_FromWideChar(Py_GetPythonHome(), -1);
    PySys_SetObject(const_cast<char*>("exec_prefix"), exec_prefix);
    Py_XDECREF(exec_prefix);

    PySys_SetArgv( commandLineArgsWide.size(), const_cast<wchar_t**>(&commandLineArgsWide[0]) );

    PyObject* mainModule = PyImport_ImportModule("__main__");

#if PY_VERSION_HEX < 0x03070000
    PyEval_InitThreads();
#endif

    std::string err;
#if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
    /// print info about python lib
    {
        printf( "PATH is %s\n", Py_GETENV("PATH") );
        printf( "PYTHONPATH is %s\n", Py_GETENV("PYTHONPATH") );
        printf( "PYTHONHOME is %s\n", Py_GETENV("PYTHONHOME") );
        printf( "Py_DebugFlag is %d\n", Py_DebugFlag );
        printf( "Py_VerboseFlag is %d\n", Py_VerboseFlag );
        printf( "Py_InteractiveFlag is %d\n", Py_InteractiveFlag );
        printf( "Py_InspectFlag is %d\n", Py_InspectFlag );
        printf( "Py_OptimizeFlag is %d\n", Py_OptimizeFlag );
        printf( "Py_NoSiteFlag is %d\n", Py_NoSiteFlag );
        printf( "Py_BytesWarningFlag is %d\n", Py_BytesWarningFlag );
        printf( "Py_FrozenFlag is %d\n", Py_FrozenFlag );
        printf( "Py_HashRandomizationFlag is %d\n", Py_HashRandomizationFlag );
        printf( "Py_IsolatedFlag is %d\n", Py_IsolatedFlag );
        printf( "Py_QuietFlag is %d\n", Py_QuietFlag );
        printf( "Py_IgnoreEnvironmentFlag is %d\n", Py_IgnoreEnvironmentFlag );
        printf( "Py_DontWriteBytecodeFlag is %d\n", Py_DontWriteBytecodeFlag );
        printf( "Py_NoUserSiteDirectory is %d\n", Py_NoUserSiteDirectory );
        printf( "Py_GetProgramName is %ls\n", Py_GetProgramName() );
        printf( "Py_GetPrefix is %ls\n", Py_GetPrefix() );
        printf( "Py_GetExecPrefix is %ls\n", Py_GetPrefix() );
        printf( "Py_GetProgramFullPath is %ls\n", Py_GetProgramFullPath() );
        printf( "Py_GetPath is %ls\n", Py_GetPath() );
        printf( "Py_GetPythonHome is %ls\n", Py_GetPythonHome() );

#define DUMP_SYS(NAME) \
            do { \
                obj = PySys_GetObject(#NAME); \
                PySys_FormatStderr("  sys.%s = ", #NAME); \
                if (obj != NULL) { \
                    PySys_FormatStdout("%A", obj); \
                } \
                else { \
                    PySys_WriteStdout("(not set)"); \
                } \
                PySys_FormatStdout("\n"); \
            } while (0)

        PyObject *obj;
        DUMP_SYS(version);
        DUMP_SYS(_base_executable);
        DUMP_SYS(base_prefix);
        DUMP_SYS(base_exec_prefix);
        DUMP_SYS(platlibdir);
        DUMP_SYS(executable);
        DUMP_SYS(prefix);
        DUMP_SYS(exec_prefix);
#undef DUMP_SYS

        PyObject *sys_path = PySys_GetObject("path");  /* borrowed reference */
        if (sys_path != NULL && PyList_Check(sys_path)) {
            PySys_WriteStdout("  sys.path = [\n");
            Py_ssize_t len = PyList_GET_SIZE(sys_path);
            for (Py_ssize_t i=0; i < len; i++) {
                PyObject *path = PyList_GET_ITEM(sys_path, i);
                PySys_FormatStdout("    %A,\n", path);
            }
            PySys_WriteStdout("  ]\n");
        }

        PyObject* dict = PyModule_GetDict(mainModule);

        PyErr_Clear();

        ///This is faster than PyRun_SimpleString since is doesn't call PyImport_AddModule("__main__")
#if PY_VERSION_HEX >= 0x030C0000
        std::string script("import sysconfig; print('Python library is in ' + sysconfig.get_path('purelib'))");
#else
        std::string script("from distutils.sysconfig import get_python_lib; print('Python library is in ' + get_python_lib())");
#endif
        PyObject* v = PyRun_String(script.c_str(), Py_file_input, dict, 0);
        if (v) {
            Py_DECREF(v);
        }
    }
#endif

    // Release the GIL, because PyEval_InitThreads acquires the GIL
    // see https://docs.python.org/3.7/c-api/init.html#c.PyEval_InitThreads
    PyThreadState *_save = PyEval_SaveThread();

    return mainModule;
#endif // PY_VERSION_HEX >= 0x030D0000
} // initializePython


std::string
PyStringToStdString(PyObject* py_val)
{
    ///Must be locked
    assert( PyThreadState_Get() );
    std::string val;
    PyObject* s = nullptr;
    // The following should work with Python 2 and 3.
    // https://stackoverflow.com/a/38600095
    if (!py_val) {
        val = "(null)";
    } else if( PyUnicode_Check(py_val) ) {  // python3 has unicode, but we convert to bytes
        s = PyUnicode_AsUTF8String(py_val);
    } else if( PyBytes_Check(py_val) ) {  // python2 has bytes already
        s = PyObject_Bytes(py_val);
    } else {
        // Not a string => Error, warning ...
        val = "(not a string)";
    }

    // If succesfully converted to bytes, then convert to C++ string
    if (s) {
        val = std::string( PyBytes_AS_STRING(s) );
        Py_XDECREF(s);
    }

    return val;
}

NATRON_PYTHON_NAMESPACE_EXIT

NATRON_NAMESPACE_EXIT
