/*
 * ===========================================================================
 * Loom SDK
 * Copyright 2011, 2012, 2013
 * The Game Engine Company, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ===========================================================================
 */

#include "zlib.h"

#include "loom/common/core/allocator.h"
#include "loom/common/core/assert.h"
#include "loom/common/core/log.h"
#include "loom/common/utils/utByteArray.h"
#include "loom/common/utils/utString.h"
#include "loom/common/platform/platformIO.h"
#include "loom/script/runtime/lsRuntime.h"
#include "loom/script/runtime/lsLuaState.h"
#include "loom/script/runtime/lsTypeValidatorRT.h"
#include "loom/script/runtime/lsProfiler.h"
#include "loom/script/native/lsLuaBridge.h"
#include "loom/script/reflection/lsType.h"
#include "loom/script/reflection/lsFieldInfo.h"
#include "loom/script/reflection/lsPropertyInfo.h"
#include "loom/script/common/lsError.h"
#include "loom/script/common/lsFile.h"
#include "loom/script/serialize/lsBinReader.h"

extern "C" {
    int luaopen_socket_core(lua_State *L);
    void luaL_openlibs(lua_State *L);
}


namespace LS {
void lsr_classinitializestatic(lua_State *L, Type *type);

utHashTable<utPointerHashKey, LSLuaState *> LSLuaState::toLuaState;

utArray<utString> LSLuaState::commandLine;

double            LSLuaState::uniqueKey      = 1;
lua_State         *LSLuaState::lastState     = NULL;
LSLuaState        *LSLuaState::lastLSState   = NULL;
double            LSLuaState::constructorKey = 0;
utArray<utString> LSLuaState::buildCache;

lmDefineLogGroup(gLuaStateLogGroup, "LuaState", true, LoomLogInfo);

// traceback stack queries
struct stackinfo
{
    const char *source;
    int        linenumber;
    MethodBase *methodBase;
};

static utStack<stackinfo> _tracestack;
static char               _tracemessage[2048];

size_t LSLuaState::allocatedBytes = 0;

static void *lsLuaAlloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;  /* not used */
    
    LSLuaState::allocatedBytes += nsize - osize;

    if (nsize == 0) 
    {
        lmFree(NULL, ptr);
        return NULL;
    }
    else if (ptr == NULL)
    {
        return lmAlloc(NULL, nsize);
    }
    else
    {
        return lmRealloc(NULL, ptr, nsize);
    }
}

void LSLuaState::open()
{
    assert(!L);

    #if LOOM_PLATFORM_64BIT
    L = luaL_newstate();
    #else
    L = lua_newstate(lsLuaAlloc, this);
    #endif

    toLuaState.insert(L, this);

    // Stop the GC initially
    lua_gc(L, LUA_GCSTOP, 0);

    // open all the standard libraries
    luaL_openlibs(L);

    // open socket library
    luaopen_socket_core(L);

    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXCLASSES);

    lua_newtable(L);
    lua_setglobal(L, "__ls_nativeclasses");

    lua_pushcfunction(L, traceback);
    lua_setglobal(L, "__ls_traceback");
    _tracemessage[0] = 0;

    // entry -> version
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXMANAGEDVERSION);

    // entry -> native user data
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXMANAGEDUSERDATA);

    // native user data -> script instance
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXMANAGEDNATIVESCRIPT);

    // native delegate table
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXNATIVEDELEGATES);

    // interned field name lookup
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXMEMBERINFONAME);

    // typeid -> type*
    lua_newtable(L);
    lua_rawseti(L, LUA_GLOBALSINDEX, LSASSEMBLYLOOKUP);

    // lua/luacfunction -> MethodBase* lookups
    lua_newtable(L);

    // weak key metatable
    lua_newtable(L);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);

    lua_rawseti(L, LUA_GLOBALSINDEX, LSINDEXMETHODLOOKUP);

    lsr_instanceregister(L);

    NativeInterface::registerNativeTypes(L);
}


void LSLuaState::close()
{
    assert(L);

    if (lastState == L)
    {
        lastState   = NULL;
        lastLSState = NULL;
    }

    // ensure profiler is down
    LSProfiler::disable(L);

    for (UTsize i = 0; i < assemblies.size(); i++)
    {
        lmDelete(NULL, assemblies.at(i));
    }

    NativeInterface::shutdownLuaState(L);

    lua_close(L);

    toLuaState.remove(L);

    L = NULL;
}


Assembly *LSLuaState::loadTypeAssembly(const utString& assemblyString)
{
    beginAssemblyLoad();

    Assembly *assembly = Assembly::loadFromString(this, assemblyString);

    utArray<Type *> types;
    assembly->getTypes(types);
    cacheAssemblyTypes(assembly, types);

    endAssemblyLoad();

    return assembly;
}


void LSLuaState::declareLuaTypes(const utArray<Type *>& types)
{
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types[i];
        if (type->getMissing()) continue;

        declareClass(type);
    }

    // validate/initialize native types
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types.at(i);
        if (type->getMissing()) continue;

        if (type->isNative() || type->hasStaticNativeMember())
        {
            NativeTypeBase *ntb = NativeInterface::getNativeType(type);

            if (!ntb)
            {
                LSError("Unable to get NativeTypeBase for type %s", type->getFullName().c_str());
            }

            if (type->isNativeManaged() != ntb->isManaged())
            {
                if (type->isNativeManaged())
                {
                    LSError("Managed mismatch for type %s, script declaration specifies managed while native bindings are unmanaged", type->getFullName().c_str());
                }
                else
                {
                    LSError("Managed mismatch for type %s, script declaration specifies unmanaged while native bindings are managed", type->getFullName().c_str());
                }
            }

            ntb->validate(type);
            type->setCTypeName(ntb->getCTypeName());
        }
    }
}


void LSLuaState::initializeLuaTypes(const utArray<Type *>& types)
{
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types[i];
        if (type->getMissing()) continue;

        type->cache();
    }

    // initialize all classes
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types[i];
        if (type->getMissing()) continue;

        initializeClass(type);
    }

    // run static initializers now that all classes have been initialized
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types[i];
        if (type->getMissing()) continue;

        lsr_classinitializestatic(VM(), type);
    }
}


void LSLuaState::cacheAssemblyTypes(Assembly *assembly, utArray<Type *>& types)
{
    // setup assembly type lookup field
    lua_rawgeti(L, LUA_GLOBALSINDEX, LSASSEMBLYLOOKUP);
    lua_pushlightuserdata(L, assembly);
    lua_setfield(L, -2, assembly->getUniqueId().c_str());
    lua_pop(L, 1);

    lmAssert(assembly->ordinalTypes == NULL, "Assembly types cache error, ordinalTypes already exists");

    assembly->ordinalTypes = lmNew(NULL) utArray<Type*>();
    assembly->ordinalTypes->resize(types.size() + 1);

    for (UTsize j = 0; j < types.size(); j++)
    {
        Type *type = types.at(j);

        assembly->types.insert(type->getName(), type);

        lmAssert(type->getTypeID() > 0 && type->getTypeID() <= (LSTYPEID)types.size(), "LSLuaState::cacheAssemblyTypes TypeID out of range");

        assembly->ordinalTypes->ptr()[type->getTypeID()] = type;

        const char *typeName = type->getFullName().c_str();

        // fast access cache
        if (!strcmp(typeName, "system.Object"))
        {
            objectType = type;
        }
        else if (!strcmp(typeName, "system.Null"))
        {
            nullType = type;
        }
        else if (!strcmp(typeName, "system.Boolean"))
        {
            booleanType = type;
        }
        else if (!strcmp(typeName, "system.Number"))
        {
            numberType = type;
        }
        else if (!strcmp(typeName, "system.String"))
        {
            stringType = type;
        }
        else if (!strcmp(typeName, "system.Function"))
        {
            functionType = type;
        }
        else if (!strcmp(typeName, "system.Vector"))
        {
            vectorType = type;
        }
        else if (!strcmp(typeName, "system.reflection.Type"))
        {
            reflectionType = type;
        }

        lua_rawgeti(L, LUA_GLOBALSINDEX, LSINDEXMEMBERINFONAME);
        lua_pushlightuserdata(L, type);
        lua_gettable(L, -2);

        // cache all members for fast lookup of memberinfo -> pre-interned
        // lua string (interning strings is the devil's work)
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);

            utArray<MemberInfo *> members;
            MemberTypes           types;
            types.method   = true;
            types.field    = true;
            types.property = true;
            type->findMembers(types, members, false);

            // cache the type to member info table
            lua_pushlightuserdata(L, type);
            lua_pushstring(L, type->getName());
            lua_settable(L, -3);

            for (UTsize i = 0; i < members.size(); i++)
            {
                MemberInfo *mi = members.at(i);

                lua_pushlightuserdata(L, mi);
                lua_pushstring(L, mi->getName());
                lua_settable(L, -3);
            }
        }
        else
        {
            lua_pop(L, 1);
        }

        lua_pop(L, 1);

        // if we weren't cached during assembly load, cache now
        if (!typeCache.get(type->getFullName()))
        {
            typeCache.insert(type->getFullName(), type);
        }
    }

    lmAssert(nullType, "LSLuaState::cacheAssemblyTypes - system.Null not found");
    lmAssert(booleanType, "LSLuaState::cacheAssemblyTypes - system.Boolean not found");
    lmAssert(numberType, "LSLuaState::cacheAssemblyTypes - system.Number not found");
    lmAssert(stringType, "LSLuaState::cacheAssemblyTypes - system.String not found");
    lmAssert(functionType, "LSLuaState::cacheAssemblyTypes - system.Function not found");
    lmAssert(reflectionType, "LSLuaState::cacheAssemblyTypes - system.reflection.Type not found");
    lmAssert(vectorType, "LSLuaState::cacheAssemblyTypes - system.Vector not found");
}

// Mark the types the provided type is imported in as missing
static void markImportedMissing(utArray<Type *> &types, Type *missing)
{
    for (UTsize i = 0; i < types.size(); i++)
    {
        Type *type = types[i];

        if (type->getMissing()) continue;

        utArray<Type *> imports;
        type->getImports(imports);

        for (UTsize im = 0; im < imports.size(); im++)
        {
            Type *import = imports[im];
            if (import == missing) {
                type->setMissing("missing import %s", missing->getFullName().c_str());
                markImportedMissing(types, type);
                break;
            }
        }
    }
}

void LSLuaState::finalizeAssemblyLoad(Assembly *assembly, utArray<Type *>& types)
{
    for (UTsize j = 0; j < types.size(); j++)
    {
        Type *type = types.at(j);

        if (type->isNative() || type->hasStaticNativeMember())
        {
            // we're native
            NativeInterface::resolveScriptType(type);
        }
    }

    bool shrink = false;
    // Runs over all types and finds out which ones
    // are incomplete (e.g. with a missing method)
    for (UTsize j = 0; j < types.size(); j++)
    {
        Type *type = types.at(j);

        // Marks subtypes of missing types as incomplete/missing
        bool incomplete = false;
        Type *search = type;
        while (search) {
            if (search->getMissing())
            {
                incomplete = true;
                break;
            }
            search = search->getBaseType();
        }

        utArray<Type *> imports;
        type->getImports(imports);

        // Marks types with missing imports as incomplete/missing
        for (UTsize im = 0; im < imports.size(); im++)
        {
            Type *import = imports[im];
            if (import->getMissing()) {
                incomplete = true;
            }
        }

        if (incomplete)
        {
            shrink = true;
            type->setMissing("incomplete");
            /// Recursively marks types that import this missing type
            // as incomplete/missing
            markImportedMissing(types, type);
        }
    }

    // Removes and deletes all missing types and moves
    // non-missing types in their place, then shrinks
    // the type array to fit
    if (shrink)
    {
        UTsize firstFree = 0;
        for (UTsize j = 0; j < types.size(); j++) {
            Type *type = types[j];
            if (!type->getMissing()) {
                types[firstFree] = type;
                firstFree++;
            }
            else {
                Module* module = const_cast<Module*>(type->getModule());
                module->removeType(type);
                lmDelete(NULL, type);
                types[j] = NULL;
            }
        }
        types.resize(firstFree);
    }

    declareLuaTypes(types);
    initializeLuaTypes(types);

    // we avoid runtime validation on mobile, this works but should be unnecessary
    // as issues with be caught on OSX/WINDOWS development platforms
#if LOOM_PLATFORM == LOOM_PLATFORM_OSX || LOOM_PLATFORM == LOOM_PLATFORM_WIN32
    for (UTsize j = 0; j < types.size(); j++)
    {
        Type            *type = types.at(j);
        TypeValidatorRT tv(this, type);
        tv.validate();
    }
#endif

    assembly->bootstrap();
}


Assembly *LSLuaState::loadAssemblyJSON(const utString& json)
{
    beginAssemblyLoad();

    Assembly *assembly = Assembly::loadFromString(this, json);

    utArray<Type *> types;
    assembly->getTypes(types);

    cacheAssemblyTypes(assembly, types);

    if (!isCompiling())
    {
        finalizeAssemblyLoad(assembly, types);
    }

    endAssemblyLoad();

    return assembly;
}


Assembly *LSLuaState::loadAssemblyBinary(utByteArray *bytes)
{
    loadAssemblyBinaryHeader(bytes);
    return loadAssemblyBinaryBody();
}

void LSLuaState::loadAssemblyBinaryHeader(utByteArray *bytes)
{
    Assembly::loadBinaryHeader(this, bytes);
}

Assembly *LSLuaState::loadAssemblyBinaryBody()
{
    return Assembly::loadBinaryBody();
}

static utString getPathFromName(const utString& assemblyName, bool absPath)
{
    // executables always in bin
    utString filePath;

    if (!absPath)
    {
        filePath = "./bin/";
    }

    filePath += assemblyName;

    if (!strstr(filePath.c_str(), ".loom"))
    {
        filePath += ".loom";
    }

    return filePath;
}

Assembly *LSLuaState::loadExecutableAssembly(const utString& assemblyName, bool absPath)
{
    utByteArray *bytes = openExecutableAssembly(assemblyName, absPath);
    readExecutableAssemblyBinaryHeader(bytes);
    Assembly *assembly = readExecutableAssemblyBinaryBody();
    closeExecutableAssembly(assemblyName, absPath, bytes);
    return assembly;
}

utByteArray *LSLuaState::openExecutableAssembly(const utString& assemblyName, bool absPath)
{
    utString filePath = getPathFromName(assemblyName, absPath);

    const char *buffer = NULL;
    long       bufferSize;
    LSMapFile(filePath.c_str(), (void **)&buffer, &bufferSize);

    lmAssert(buffer && bufferSize, "Error loading executable: %s, unable to map file", assemblyName.c_str());

    return openExecutableAssemblyBinary(buffer, bufferSize);
}

void LSLuaState::closeExecutableAssembly(const utString& assemblyName, bool absPath, utByteArray *bytes)
{
    utString filePath = getPathFromName(assemblyName, absPath);
    LSUnmapFile(filePath.c_str());
    closeExecutableAssemblyBinary(bytes);
}

Assembly *LSLuaState::loadExecutableAssemblyBinary(const char *buffer, long bufferSize) {
    utByteArray *bytes = openExecutableAssemblyBinary(buffer, bufferSize);
    Assembly *assembly = readExecutableAssemblyBinary(bytes);
    closeExecutableAssemblyBinary(bytes);
    return assembly;
}

utByteArray *LSLuaState::openExecutableAssemblyBinary(const char *buffer, long bufferSize) {

    utByteArray headerBytes;

    headerBytes.allocateAndCopy((void *)buffer, sizeof(unsigned int) * 4);

    // we need to decompress
    lmCheck(headerBytes.readUnsignedInt() == LOOM_BINARY_ID, "binary id mismatch");
    lmCheck(headerBytes.readUnsignedInt() == LOOM_BINARY_VERSION_MAJOR, "major version mismatch");
    lmCheck(headerBytes.readUnsignedInt() == LOOM_BINARY_VERSION_MINOR, "minor version mismatch");
    unsigned int sz = headerBytes.readUnsignedInt();

    utByteArray *bytes = lmNew(NULL) utByteArray();
    bytes->resize(sz);

    uLongf readSZ = sz;

    int ok = uncompress((Bytef *)bytes->getDataPtr(), (uLongf *)&readSZ, (const Bytef *)((unsigned char *)buffer + sizeof(unsigned int) * 4), (uLong)sz);

    lmCheck(ok == Z_OK, "problem uncompressing executable assembly");
    lmCheck(readSZ == sz, "Read size mismatch");

    return bytes;
}


Assembly *LSLuaState::readExecutableAssemblyBinary(utByteArray *bytes) {
    return loadAssemblyBinary(bytes);
}

void LSLuaState::readExecutableAssemblyBinaryHeader(utByteArray *bytes) {
    loadAssemblyBinaryHeader(bytes);
}
Assembly *LSLuaState::readExecutableAssemblyBinaryBody() {
    Assembly *assembly = loadAssemblyBinaryBody();
    lmAssert(assembly, "Error loading executable");
    assembly->freeByteCode();
    return assembly;
}

void LSLuaState::closeExecutableAssemblyBinary(utByteArray *bytes) {
    lmDelete(NULL, bytes);
}


// get all types loaded for a given package
void LSLuaState::getPackageTypes(const utString&  packageName,
                                 utArray<Type *>& types)
{
    for (UTsize i = 0; i < assemblies.size(); i++)
    {
        Assembly *assembly = assemblies.at(i);

        assembly->getPackageTypes(packageName, types);
    }
}


Assembly *LSLuaState::getAssembly(const utString& name)
{
    for (UTsize i = 0; i < assemblies.size(); i++)
    {
        Assembly *assembly = assemblies.at(i);

        if (assembly->getName() == name)
        {
            return assembly;
        }

        if (assembly->getName() + ".loom" == name)
        {
            return assembly;
        }
    }

    return NULL;
}

Assembly *LSLuaState::getAssemblyByUID(const utString& uid)
{
    for (UTsize i = 0; i < assemblies.size(); i++)
    {
        Assembly *assembly = assemblies.at(i);

        if (assembly->getUniqueId() == uid)
        {
            return assembly;
        }
    }

    return NULL;
}

void LSLuaState::invokeStaticMethod(const utString& typePath,
                                    const char *methodName, int numParameters)
{
    Type *type = getType(typePath.c_str());

    lmAssert(type, "LSLuaState::invokeStaticMethod unknown type: %s", typePath.c_str());

    MemberInfo *member = type->findMember(methodName);
    lmAssert(member, "LSLuaState::invokeStaticMethod unknown member: %s:%s", typePath.c_str(), methodName);
    if (!member->isMethod())
    {
        lmAssert(0, "LSLuaState::invokeStaticMethod member: %s:%s is not a method", typePath.c_str(), methodName);
    }

    MethodInfo *method = (MethodInfo *)member;

    lmAssert(method->isStatic(), "LSLuaState::invokeStaticMethod member: %s:%s is not a static method", typePath.c_str(), methodName);

    method->invoke(NULL, numParameters);
}


void LSLuaState::getClassTable(Type *type)
{
    lsr_getclasstable(L, type);
}


void LSLuaState::declareClass(Type *type)
{
    lsr_declareclass(L, type);
}


void LSLuaState::initializeClass(Type *type)
{
    lsr_classinitialize(L, type);
}


void LSLuaState::tick()
{
    invokeStaticMethod("system.VM", "_tick");
}


void LSLuaState::initCommandLine(int argc, const char **argv)
{
    for (int i = 0; i < argc; i++)
    {
        commandLine.push_back(argv[i]);
    }
}

void LSLuaState::initCommandLine(const utArray<utString>& args)
{
    commandLine = args;
}

void LSLuaState::dumpManagedNatives()
{
    NativeInterface::dumpManagedNatives(L);
}

static utString getLuaValue(lua_State *L, int index)
{
    int t = lua_type(L, index);
    switch (t) {
        case LUA_TSTRING:  return utStringFormat("\"%s\"", lua_tostring(L, index)); break;
        case LUA_TNUMBER:  return utStringFormat("%.0f", lua_tonumber(L, index)); break;
        case LUA_TBOOLEAN: return utString(lua_toboolean(L, index) ? "true" : "false"); break;
        case LUA_TFUNCTION:
            lua_Debug info;
            lua_pushvalue(L, index);
            lua_getinfo(L, ">Snlu", &info);
            return utStringFormat("\
function \
src %s, short %s, linedef %d, lastlinedef %d, what %s, \
name %s, namewhat %s, curline %d, nups %d",
info.source, info.short_src, info.linedefined, info.lastlinedefined, info.what,
info.name, info.namewhat, info.currentline, info.nups
            );
            break;
    }
    return utString(lua_typename(L, t));
}

void LSLuaState::dumpLuaTable(lua_State *L, int index, int levels = 2, int level)
{
    if (level >= levels) return;

    lua_pushvalue(L, index);
    lua_pushnil(L);

    //"%.*s " format, gCtorLevel, "||||||||||||||||||||||||||||||||||||||||", __VA_ARGS__);

    const int keyIndex = -2;
    const int valueIndex = -1;

    utString indent = "";
    for (int i = 0; i < level+1; i++) indent = indent + "    ";

    while (lua_next(L, -2) != 0)
    {
        utString key = getLuaValue(L, keyIndex);
        utString value = getLuaValue(L, valueIndex);
        int valueType = lua_type(L, valueIndex);
        lmLog(gLuaStateLogGroup, "%s%s: %s", indent.c_str() , key.c_str(), value.c_str());
        if (valueType == LUA_TTABLE) {
            dumpLuaTable(L, valueIndex, levels, level + 1);
        }

        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}

void LSLuaState::dumpLuaStack(lua_State *L)
{
    int i;
    int top = lua_gettop(L);

    lmLog(gLuaStateLogGroup, "Total in stack: %d", top);

    for (i = 1; i <= top; i++)
    {
        int t = lua_type(L, i);
        switch (t) {
        case LUA_TTABLE:
            lmLog(gLuaStateLogGroup, "%d: table", i);
            dumpLuaTable(L, i, 0);
            break;
        default:
            lmLog(gLuaStateLogGroup, "%d: %s", i, getLuaValue(L, i).c_str());
            break;
        }
    }
    lmLog(gLuaStateLogGroup, "");
}

void LSLuaState::dumpLuaStack()
{
    LSLuaState::dumpLuaStack(L);
}

int LSLuaState::getStackSize()
{
    return L->stacksize;
}


static void _getCurrentStack(lua_State *L, utStack<stackinfo>& stack)
{
    int       top = lua_gettop(L);
    lua_Debug lstack;
    int       stackFrame = 0;

    MethodBase *lastMethod = NULL;

    while (true)
    {
        // if we get a null result here, we are out of stack
        if (!lua_getstack(L, stackFrame++, &lstack))
        {
            lua_settop(L, top);
            return;
        }

        // something bad in denmark
        if (!lua_getinfo(L, "fSl", &lstack))
        {
            lua_settop(L, top);
            return;
        }

        bool cfunc = false;
        if (lua_iscfunction(L, -1))
        {
            cfunc = true;
        }

        lua_rawgeti(L, LUA_GLOBALSINDEX, LSINDEXMETHODLOOKUP);
        lua_pushvalue(L, -2);
        lua_rawget(L, -2);

        if (lua_isnil(L, -1))
        {
            lua_settop(L, top);
            continue;
        }

        MethodBase *methodBase = (MethodBase *)lua_topointer(L, -1);

        lua_settop(L, top);

        // we only want the root call, not the pcall wrapper
        if (cfunc && (lastMethod == methodBase))
        {
            continue;
        }

        lastMethod = methodBase;

        stackinfo si;
        si.methodBase = methodBase;
        si.source     = methodBase->isNative() ? "[NATIVE]" : lstack.source;
        si.linenumber = lstack.currentline == -1 ? 0 : lstack.currentline;

        stack.push(si);
    }
}


int LSLuaState::traceback(lua_State *L)
{
    _tracestack.clear();

    if (lua_isstring(L, 1))
    {
        snprintf(_tracemessage, 2040, "%s", lua_tostring(L, 1));
    }
    else
    {
        _tracemessage[0] = 0;
    }

    _getCurrentStack(L, _tracestack);

    return 0;
}


void LSLuaState::triggerRuntimeError(const char *format, ...)
{
    LSLog(LSLogError, "=====================");
    LSLog(LSLogError, "=   RUNTIME ERROR   =");
    LSLog(LSLogError, "=====================\n");

    lmAllocVerifyAll();

    dumpLuaStack();

    char    buff[2048];
    va_list args;
    va_start(args, format);
#ifdef _MSC_VER
    vsprintf_s(buff, 2046, format, args);
#else
    vsnprintf(buff, 2046, format, args);
#endif
    va_end(args);

    if (buff)
    {
        LSLog(LSLogError, "%s", buff);
    }

    if (_tracemessage[0])
    {
        LSLog(LSLogError, "%s\n", _tracemessage);
    }

    _tracemessage[0] = 0;

    // coming from a native assert?
    if (!_tracestack.size())
    {
        _getCurrentStack(L, _tracestack);
    }

    LSLog(LSLogError, "Stacktrace:", buff);

    for (UTsize i = 0; i < _tracestack.size(); i++)
    {
        const stackinfo& s = _tracestack.peek(_tracestack.size() - i - 1);

        LSLog(LSLogError, "%s : %s : %i", s.methodBase->getFullMemberName(),
              s.source ? s.source : NULL, s.linenumber);
    }
    
    LSError("\nFatal Runtime Error\n\n");

}
}
