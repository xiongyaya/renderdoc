/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2016 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <tchar.h>
#include <tlhelp32.h>
#include <string>
#include "core/core.h"
#include "os/os_specific.h"
#include "serialise/string_utils.h"

using std::string;

static wstring lowercase(wstring in)
{
  wstring ret;
  ret.resize(in.size());
  for(size_t i = 0; i < ret.size(); i++)
    ret[i] = towlower(in[i]);
  return ret;
}

static vector<Process::EnvironmentModification> &GetEnvModifications()
{
  static vector<Process::EnvironmentModification> envCallbacks;
  return envCallbacks;
}

static map<wstring, string> EnvStringToEnvMap(const wchar_t *envstring)
{
  map<wstring, string> ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    wstring name;
    wstring value;

    name.assign(e, equals);
    value = equals + 1;

    // set all names to lower case so we can do case-insensitive lookups
    ret[lowercase(name)] = StringFormat::Wide2UTF8(value);

    e += name.size();     // jump to =
    e++;                  // advance past it
    e += value.size();    // jump to \0
    e++;                  // advance past it
  }

  return ret;
}

void Process::RegisterEnvironmentModification(Process::EnvironmentModification modif)
{
  GetEnvModifications().push_back(modif);
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  map<wstring, string> currentEnv = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  vector<EnvironmentModification> &modifications = GetEnvModifications();

  for(size_t i = 0; i < modifications.size(); i++)
  {
    EnvironmentModification &m = modifications[i];

    // set all names to lower case so we can do case-insensitive lookups, but
    // preserve the original name so that added variables maintain the same case
    wstring name = StringFormat::UTF82Wide(m.name);
    wstring lowername = lowercase(name);

    string value;

    auto it = currentEnv.find(lowername);
    if(it != currentEnv.end())
    {
      value = it->second;
      name = lowername;
    }

    switch(m.type)
    {
      case eEnvModification_Replace: value = m.value; break;
      case eEnvModification_Append: value += m.value; break;
      case eEnvModification_AppendColon:
        if(!value.empty())
          value += ":";
        value += m.value;
        break;
      case eEnvModification_AppendPlatform:
      case eEnvModification_AppendSemiColon:
        if(!value.empty())
          value += ";";
        value += m.value;
        break;
      case eEnvModification_Prepend: value = m.value + value; break;
      case eEnvModification_PrependColon:
        if(!value.empty())
          value = m.value + ":" + value;
        else
          value = m.value;
        break;
      case eEnvModification_PrependPlatform:
      case eEnvModification_PrependSemiColon:
        if(!value.empty())
          value = m.value + ";" + value;
        else
          value = m.value;
        break;
      default: RDCERR("Unexpected environment modification type");
    }

    SetEnvironmentVariableW(name.c_str(), StringFormat::UTF82Wide(value).c_str());
  }

  // these have been applied to the current process
  modifications.clear();
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl RENDERDOC_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_SetLogFile(const char *log)
{
  if(log)
    RenderDoc::Inst().SetLogFile(log);
}

static Process::EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_EnvMod(Process::ModificationType *type)
{
  if(type)
  {
    tempEnvMod.type = *type;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl RENDERDOC_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

void InjectDLL(HANDLE hProcess, wstring libName)
{
  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return;
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(remoteMem)
  {
    WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, sizeof(dllPath), NULL);

    HANDLE hThread = CreateRemoteThread(
        hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW"),
        remoteMem, 0, NULL);
    if(hThread)
    {
      WaitForSingleObject(hThread, INFINITE);
      CloseHandle(hThread);
    }
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  }
  else
  {
    RDCERR("Couldn't allocate remote memory for DLL '%ls'", libName.c_str());
  }
}

uintptr_t FindRemoteDLL(DWORD pid, wstring libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  libName = strlower(libName);

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, libName.c_str()))
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%ls' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

void InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_DLL_FILE) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  HANDLE hThread =
      CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
  WaitForSingleObject(hThread, INFINITE);

  ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
}

static PROCESS_INFORMATION RunProcess(const char *app, const char *workingDir, const char *cmdLine)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  wstring workdir = L"";

  if(workingDir != NULL && workingDir[0] != 0)
    workdir = StringFormat::UTF82Wide(string(workingDir));
  else
    workdir = StringFormat::UTF82Wide(dirname(string(app)));

  wchar_t *paramsAlloc = NULL;

  wstring wapp = StringFormat::UTF82Wide(string(app));

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  wstring wcmd = L"";

  if(cmdLine != NULL && cmdLine[0] != 0)
  {
    wcmd = StringFormat::UTF82Wide(string(cmdLine));
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(cmdLine != NULL && cmdLine[0] != 0)
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  RDCLOG("Running process %s", app);

  BOOL retValue = CreateProcessW(NULL, paramsAlloc, &pSec, &tSec, false,
                                 CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL,
                                 workdir.c_str(), &si, &pi);

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    RDCERR("Process %s could not be loaded.", app);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }

  return pi;
}

uint32_t Process::InjectIntoProcess(uint32_t pid, EnvironmentModification *env, const char *logfile,
                                    const CaptureOptions *opts, bool waitForExit)
{
  CaptureOptions options;
  if(opts)
    options = *opts;

  wstring wlogfile = logfile == NULL ? L"" : StringFormat::UTF82Wide(logfile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(options.DelayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > options.DelayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", options.DelayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_DLL_FILE) ".dll"), &renderdocPath[0],
                     MAX_PATH - 1);

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDCERR("Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return 0;
  }

#if !defined(WIN64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDCERR("Couldn't determine bitness of self, err: %08x", err);
    return 0;
  }

  if(selfWow64 && !isWow64)
  {
    RDCERR("Can't capture x64 process with x86 renderdoc");
    CloseHandle(hProcess);
    return 0;
  }
#else
  // farm off to x86 version
  if(isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash)
      *slash = 0;

    wcscat_s(renderdocPath, L"\\x86\\renderdoccmd.exe");

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    string optstr;
    {
      optstr.reserve(sizeof(CaptureOptions) * 2 + 1);
      byte *b = (byte *)opts;
      for(size_t i = 0; i < sizeof(CaptureOptions); i++)
      {
        optstr.push_back(char('a' + ((b[i] >> 4) & 0xf)));
        optstr.push_back(char('a' + ((b[i]) & 0xf)));
      }
    }

    wchar_t *paramsAlloc = new wchar_t[2048];

    _snwprintf_s(paramsAlloc, 2047, 2047,
                 L"\"%ls\" cap32for64 --pid=%d --log=\"%ls\" --capopts=\"%hs\"", renderdocPath, pid,
                 wlogfile.c_str(), optstr.c_str());

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    wstring cmdWithEnv;

    if(env)
    {
      cmdWithEnv = paramsAlloc;

      for(;;)
      {
        string name = trim(env->name);
        string value = env->value;
        ModificationType type = env->type;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(type)
        {
          case eEnvModification_Replace: cmdWithEnv += L"replace "; break;

          case eEnvModification_AppendPlatform: cmdWithEnv += L"append-platform "; break;

          case eEnvModification_AppendSemiColon: cmdWithEnv += L"append-semicolon "; break;
          case eEnvModification_AppendColon: cmdWithEnv += L"append-colon "; break;
          case eEnvModification_Append: cmdWithEnv += L"append "; break;

          case eEnvModification_PrependPlatform: cmdWithEnv += L"prepend-platform "; break;

          case eEnvModification_PrependSemiColon: cmdWithEnv += L"prepend-semicolon "; break;
          case eEnvModification_PrependColon: cmdWithEnv += L"prepend-colon "; break;
          case eEnvModification_Prepend: cmdWithEnv += L"prepend "; break;
        }

        // escape the parameters
        for(auto it = name.begin(); it != name.end(); ++it)
        {
          if(*it == '"')
            it = name.insert(it, '\\') + 1;
        }

        for(auto it = value.begin(); it != value.end(); ++it)
        {
          if(*it == '"')
            it = value.insert(it, '\\') + 1;
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + StringFormat::UTF82Wide(name) + L"\" ";
        cmdWithEnv += L"\"" + StringFormat::UTF82Wide(value) + L"\" ";

        env++;
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false, CREATE_SUSPENDED, NULL,
                                   NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDCERR("Can't spawn x86 renderdoccmd - missing files?");
      return 0;
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    return (uint32_t)exitCode;
  }
#endif

  InjectDLL(hProcess, renderdocPath);

  uintptr_t loc = FindRemoteDLL(pid, CONCAT(L, STRINGIZE(RDOC_DLL_FILE)) L".dll");

  uint32_t controlident = 0;

  if(loc == 0)
  {
    RDCERR("Can't locate " STRINGIZE(RDOC_DLL_FILE) ".dll in remote PID %d", pid);
  }
  else
  {
    // safe to cast away the const as we know these functions don't modify the parameters

    if(logfile != NULL)
      InjectFunctionCall(hProcess, loc, "RENDERDOC_SetLogFile", (void *)logfile, strlen(logfile) + 1);

    if(opts != NULL)
      InjectFunctionCall(hProcess, loc, "RENDERDOC_SetCaptureOptions", (CaptureOptions *)opts,
                         sizeof(CaptureOptions));

    InjectFunctionCall(hProcess, loc, "RENDERDOC_GetTargetControlIdent", &controlident,
                       sizeof(controlident));

    if(env)
    {
      for(;;)
      {
        string name = trim(env->name);
        string value = env->value;
        ModificationType type = env->type;

        if(name == "")
          break;

        InjectFunctionCall(hProcess, loc, "RENDERDOC_EnvModName", (void *)name.c_str(),
                           name.size() + 1);
        InjectFunctionCall(hProcess, loc, "RENDERDOC_EnvModValue", (void *)value.c_str(),
                           value.size() + 1);
        InjectFunctionCall(hProcess, loc, "RENDERDOC_EnvMod", &type, sizeof(type));

        env++;
      }

      // parameter is unused
      InjectFunctionCall(hProcess, loc, "RENDERDOC_ApplyEnvMods", env,
                         sizeof(EnvironmentModification));
    }
  }

  if(waitForExit)
    WaitForSingleObject(hProcess, INFINITE);

  CloseHandle(hProcess);

  return controlident;
}

uint32_t Process::LaunchProcess(const char *app, const char *workingDir, const char *cmdLine)
{
  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine);

  if(pi.dwProcessId == 0)
  {
    RDCERR("Couldn't launch process '%s'", app);
    return 0;
  }

  RDCLOG("Launched process '%s' with '%s'", app, cmdLine);

  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchAndInjectIntoProcess(const char *app, const char *workingDir,
                                             const char *cmdLine, EnvironmentModification *env,
                                             const char *logfile, const CaptureOptions *opts,
                                             bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_DLL_FILE) ".dll"), "RENDERDOC_SetLogFile");

  if(func == NULL)
  {
    RDCERR("Can't find required export function in " STRINGIZE(
        RDOC_DLL_FILE) ".dll - corrupted/missing file?");
    return 0;
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine);

  if(pi.dwProcessId == 0)
    return 0;

  uint32_t ret = InjectIntoProcess(pi.dwProcessId, env, logfile, opts, false);

  CloseHandle(pi.hProcess);
  ResumeThread(pi.hThread);
  ResumeThread(pi.hThread);

  if(ret == 0)
  {
    CloseHandle(pi.hThread);
    return 0;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

void Process::StartGlobalHook(const char *pathmatch, const char *logfile, const CaptureOptions *opts)
{
  if(pathmatch == NULL)
    return;

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_DLL_FILE) ".dll"), &renderdocPath[0],
                     MAX_PATH - 1);

  wchar_t *slash = wcsrchr(renderdocPath, L'\\');

  if(slash)
    *slash = 0;
  else
    slash = renderdocPath + wcslen(renderdocPath);

  wcscat_s(renderdocPath, L"\\renderdoccmd.exe");

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  wchar_t *paramsAlloc = new wchar_t[2048];

  // serialise to string with two chars per byte
  string optstr;
  {
    optstr.reserve(sizeof(CaptureOptions) * 2 + 1);
    byte *b = (byte *)opts;
    for(size_t i = 0; i < sizeof(CaptureOptions); i++)
    {
      optstr.push_back(char('a' + ((b[i] >> 4) & 0xf)));
      optstr.push_back(char('a' + ((b[i]) & 0xf)));
    }
  }

  wstring wlogfile = logfile == NULL ? L"" : StringFormat::UTF82Wide(string(logfile));
  wstring wpathmatch = StringFormat::UTF82Wide(string(pathmatch));

  _snwprintf_s(paramsAlloc, 2047, 2047,
               L"\"%ls\" globalhook --match \"%ls\" --log \"%ls\" --capopts \"%hs\"", renderdocPath,
               wpathmatch.c_str(), wlogfile.c_str(), optstr.c_str());

  paramsAlloc[2047] = 0;

  BOOL retValue = CreateProcessW(NULL, paramsAlloc, &pSec, &tSec, false, 0, NULL, NULL, &si, &pi);

  if(retValue == FALSE)
    return;

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

#if defined(WIN64)
  *slash = 0;

  wcscat_s(renderdocPath, L"\\x86\\renderdoccmd.exe");

  _snwprintf_s(paramsAlloc, 2047, 2047,
               L"\"%ls\" globalhook --match \"%ls\" --log \"%ls\" --capopts \"%hs\"", renderdocPath,
               wpathmatch.c_str(), wlogfile.c_str(), optstr.c_str());

  paramsAlloc[2047] = 0;

  retValue = CreateProcessW(NULL, paramsAlloc, &pSec, &tSec, false, 0, NULL, NULL, &si, &pi);

  if(retValue == FALSE)
    return;

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif
}

void *Process::LoadModule(const char *module)
{
  HMODULE mod = GetModuleHandleA(module);
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module);
}

void *Process::GetFunctionAddress(void *module, const char *function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function);
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}
