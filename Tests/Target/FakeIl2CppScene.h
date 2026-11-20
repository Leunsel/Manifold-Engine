#pragma once

#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" __declspec(dllexport) volatile long g_il2cppFrames = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppRenders = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppInvokes = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppOffThread = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppHandlesLive = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppHandleErrors = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppProbesMade = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppProbesLive = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppRunInBackground = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppPaused = 0;

namespace
{
    bool g_scene = false;
    bool g_sceneAlt = false;
    DWORD g_sceneThread = 0;

    using FakeImpl = void* (*)(void* self, void** params, void** exc);

    struct SceneObject { const FakeIl2CppClass* Klass; void* Monitor; };
    struct SceneString { SceneObject Header; int Length; char16_t Chars[1]; };
    struct SceneArray { SceneObject Header; void* Bounds; std::size_t Length; void* Items[1]; };
    struct SceneBoxedInt { SceneObject Header; int Value; };
    struct SceneVector { float X, Y, Z; };
    struct NativeObject;
    struct UnityObject { SceneObject Header; NativeObject* CachedPtr; };
    struct HealthComponent { UnityObject Base; int Hp; float Regen; SceneString* Label; UnityObject* Target; SceneVector Spawn; int Mode; };
    struct TypeObject { SceneObject Header; const FakeIl2CppType* Type; };

    struct NativeObject
    {
        std::u16string Name;
        std::u16string Tag = u"Untagged";
        bool Active = true;
        int Layer = 0;
        int Scene = 0;
        NativeObject* Parent = nullptr;
        std::vector<NativeObject*> Children;
        UnityObject* Wrapper = nullptr;
        UnityObject* Transform = nullptr;
        std::vector<UnityObject*> Components;
        SceneVector Position{};
        SceneVector Euler{};
        SceneVector Scale{ 1.0f, 1.0f, 1.0f };
    };

    struct NativeScene
    {
        int Handle = 0;
        std::u16string Name;
        int BuildIndex = -1;
        std::vector<NativeObject*> Roots;
    };

    struct ClassBuild
    {
        std::vector<const FakeMethodInfo*> Methods;
        std::vector<FakeFieldInfo> Fields;
        std::vector<FakePropertyInfo> Properties;
    };

    FakeIl2CppClass g_unityClasses[48];
    FakeIl2CppImage g_unityImage = { "UnityEngine.CoreModule.dll", g_unityClasses, 0 };
    FakeIl2CppAssembly g_unityAssembly = { &g_unityImage };
    FakeIl2CppClass g_sceneGameClasses[8];
    FakeIl2CppImage g_sceneGameImage = { "Assembly-CSharp-firstpass.dll", g_sceneGameClasses, 0 };
    FakeIl2CppAssembly g_sceneGameAssembly = { &g_sceneGameImage };
    const FakeIl2CppAssembly* g_sceneAssemblies[] = { &kCorlibAssembly, &kGameAssembly, &g_unityAssembly, &g_sceneGameAssembly };
    FakeIl2CppClass g_arrayClasses[4];

    std::map<FakeIl2CppClass*, ClassBuild> g_builds;
    std::set<const FakeIl2CppClass*> g_sceneClassSet;
    std::map<UnityObject*, bool> g_enabled;
    std::vector<UnityObject*> g_unityObjects;
    NativeScene g_scenes[3];
    std::mutex g_handleLock;
    std::vector<void*> g_handles;
    std::map<const FakeIl2CppType*, TypeObject*> g_typeObjects;
    int g_healthStatics[4] = { 7, 0, 0, 0 };

    FakeIl2CppClass* g_objectClass = nullptr;
    FakeIl2CppClass* g_gameObjectClass = nullptr;
    FakeIl2CppClass* g_componentClass = nullptr;
    FakeIl2CppClass* g_transformClass = nullptr;
    FakeIl2CppClass* g_behaviourClass = nullptr;
    FakeIl2CppClass* g_healthClass = nullptr;
    FakeIl2CppClass* g_sceneStruct = nullptr;
    FakeIl2CppClass* g_vectorClass = nullptr;
    FakeIl2CppClass* g_int32Class = nullptr;
    FakeIl2CppClass* g_booleanClass = nullptr;
    FakeIl2CppClass* g_stringClass = nullptr;
    FakeIl2CppClass* g_typeClass = nullptr;
    FakeIl2CppClass* g_nullReference = nullptr;
    FakeIl2CppClass* g_invalidOperation = nullptr;
    FakeIl2CppClass* g_gameObjectArray = nullptr;
    FakeIl2CppClass* g_componentArray = nullptr;
    FakeIl2CppClass* g_objectArray = nullptr;
    FakeMethodInfo* g_executeTasks = nullptr;
    FakeMethodInfo* g_beforeRender = nullptr;
    FakeIl2CppType g_intPtrType = { 0x18 };
    FakeIl2CppType g_intRefType = { 0x08, nullptr, true };

    void SceneTouch()
    {
        if (::GetCurrentThreadId() != g_sceneThread)
            ::InterlockedIncrement(&g_il2cppOffThread);
    }

    template <typename T>
    T* SceneNew(const FakeIl2CppClass* klass, std::size_t extra = 0)
    {
        T* object = static_cast<T*>(std::calloc(1, sizeof(T) + extra));
        reinterpret_cast<SceneObject*>(object)->Klass = klass;
        return object;
    }

    SceneString* SceneText(const std::u16string& text)
    {
        SceneString* string = SceneNew<SceneString>(g_stringClass, text.size() * sizeof(char16_t));
        string->Length = static_cast<int>(text.size());
        std::memcpy(string->Chars, text.data(), text.size() * sizeof(char16_t));
        return string;
    }

    std::u16string SceneRead(const SceneString* string)
    {
        return string ? std::u16string(string->Chars, static_cast<std::size_t>(string->Length)) : std::u16string();
    }

    SceneArray* SceneList(const FakeIl2CppClass* klass, const std::vector<void*>& items)
    {
        SceneArray* array = SceneNew<SceneArray>(klass, items.size() * sizeof(void*));
        array->Length = items.size();
        for (std::size_t i = 0; i < items.size(); ++i)
            array->Items[i] = items[i];
        return array;
    }

    void* SceneBox(const FakeIl2CppClass* klass, const void* data, std::size_t size)
    {
        SceneObject* box = static_cast<SceneObject*>(std::calloc(1, sizeof(SceneObject) + std::max<std::size_t>(size, 8)));
        box->Klass = klass;
        std::memcpy(box + 1, data, size);
        return box;
    }

    void* BoxInt(int value) { return SceneBox(g_int32Class, &value, sizeof(value)); }
    void* BoxBool(bool value) { return SceneBox(g_booleanClass, &value, 1); }
    void* BoxVector(const SceneVector& value) { return SceneBox(g_vectorClass, &value, sizeof(value)); }
    void* BoxScene(int handle) { return SceneBox(g_sceneStruct, &handle, sizeof(handle)); }

    void* SceneThrow(void** exc, const FakeIl2CppClass* klass)
    {
        if (exc)
            *exc = SceneNew<SceneObject>(klass);
        return nullptr;
    }

    NativeObject* Native(void* self, void** exc)
    {
        UnityObject* object = static_cast<UnityObject*>(self);
        if (!object || !object->CachedPtr)
        {
            SceneThrow(exc, g_nullReference);
            return nullptr;
        }
        return object->CachedPtr;
    }

    int StructInt(void* self)
    {
        return g_sceneAlt ? static_cast<SceneBoxedInt*>(self)->Value : *static_cast<int*>(self);
    }

    NativeScene* SceneOf(void* self, void** exc)
    {
        const int handle = StructInt(self);
        for (NativeScene& scene : g_scenes)
        {
            if (scene.Handle == handle)
                return &scene;
        }
        SceneThrow(exc, g_invalidOperation);
        return nullptr;
    }

    bool SceneIsA(const FakeIl2CppClass* klass, const FakeIl2CppClass* base)
    {
        for (; klass; klass = klass->Parent)
        {
            if (klass == base)
                return true;
        }
        return false;
    }

    bool ActiveInHierarchy(const NativeObject* object)
    {
        for (; object; object = object->Parent)
        {
            if (!object->Active)
                return false;
        }
        return true;
    }

    FakeIl2CppClass* AddClass(FakeIl2CppImage& image, FakeIl2CppClass* storage, const char* space, const char* name, const FakeIl2CppClass* parent, int code,
        int valueSize = 0, bool isEnum = false)
    {
        FakeIl2CppClass* klass = &storage[image.ClassCount++];
        klass->Image = &image;
        klass->Name = name;
        klass->Namespace = space;
        klass->Parent = parent;
        klass->ValueType = valueSize > 0;
        klass->ValueSize = valueSize;
        klass->Enum = isEnum;
        klass->ByVal = { code, klass };
        g_sceneClassSet.insert(klass);
        return klass;
    }

    FakeIl2CppClass* AddArray(int index, const FakeIl2CppClass* element, const char* name)
    {
        FakeIl2CppClass* klass = &g_arrayClasses[index];
        klass->Image = element->Image;
        klass->Name = name;
        klass->Namespace = element->Namespace;
        klass->Parent = &kCorlibClasses[1];
        klass->Rank = 1;
        klass->Element = element;
        klass->ByVal = { 0x1D, klass };
        g_sceneClassSet.insert(klass);
        return klass;
    }

    const FakeIl2CppType* TypeOf(const FakeIl2CppClass* klass)
    {
        return &klass->ByVal;
    }

    FakeMethodInfo* AddMethod(FakeIl2CppClass* klass, const char* name, const FakeIl2CppType* returns, std::vector<const FakeIl2CppType*> parameters,
        std::vector<const char*> names, bool isStatic, FakeImpl impl, bool generic = false)
    {
        FakeMethodInfo* method = new FakeMethodInfo{};
        method->MethodPointer = reinterpret_cast<void*>(impl);
        method->Invoker = reinterpret_cast<void*>(impl);
        method->Name = name;
        method->Klass = klass;
        method->ReturnType = const_cast<FakeIl2CppType*>(returns);
        method->ParamCount = static_cast<unsigned>(parameters.size());
        method->Flags = isStatic ? 0x10u : 0u;
        method->Generic = generic;
        const FakeIl2CppType** types = new const FakeIl2CppType*[parameters.size() + 1]{};
        const char** labels = new const char*[parameters.size() + 1]{};
        for (std::size_t i = 0; i < parameters.size(); ++i)
        {
            types[i] = parameters[i];
            labels[i] = i < names.size() ? names[i] : "value";
        }
        method->ParamTypes = types;
        method->ParamNames = labels;
        g_builds[klass].Methods.push_back(method);
        return method;
    }

    void AddField(FakeIl2CppClass* klass, const char* name, const FakeIl2CppType* type, std::size_t offset, int flags = 0, long long literal = 0)
    {
        FakeFieldInfo field = { name, type, klass, static_cast<int>(offset), 0, flags };
        field.Literal = literal;
        g_builds[klass].Fields.push_back(field);
    }

    void AddProperty(FakeIl2CppClass* klass, const char* name, const FakeMethodInfo* get, const FakeMethodInfo* set)
    {
        g_builds[klass].Properties.push_back({ name, get, set, klass });
    }

    NativeObject* AddGameObject(int scene, NativeObject* parent, const char16_t* name)
    {
        NativeObject* native = new NativeObject();
        native->Name = name;
        native->Scene = scene;
        native->Parent = parent;
        native->Wrapper = SceneNew<UnityObject>(g_gameObjectClass);
        native->Wrapper->CachedPtr = native;
        native->Transform = SceneNew<UnityObject>(g_transformClass);
        native->Transform->CachedPtr = native;
        native->Components.push_back(native->Transform);
        g_unityObjects.push_back(native->Wrapper);
        g_unityObjects.push_back(native->Transform);
        if (parent)
            parent->Children.push_back(native);
        else
            g_scenes[scene].Roots.push_back(native);
        return native;
    }

    void SceneExecuteTasks(const FakeMethodInfo*)
    {
        ::InterlockedIncrement(&g_il2cppFrames);
    }

    void SceneBeforeRender(const FakeMethodInfo*)
    {
        ::InterlockedIncrement(&g_il2cppRenders);
    }

    void SceneFrame()
    {
        using Frame = void (*)(const FakeMethodInfo*);
        if (g_executeTasks && !g_sceneAlt)
        {
            void* volatile* entry = &g_executeTasks->MethodPointer;
            reinterpret_cast<Frame>(*entry)(g_executeTasks);
        }
        if (g_beforeRender)
        {
            void* volatile* entry = &g_beforeRender->MethodPointer;
            reinterpret_cast<Frame>(*entry)(g_beforeRender);
        }
    }

    void BuildScene()
    {
        g_sceneThread = ::GetCurrentThreadId();
        const FakeIl2CppClass* object = &kCorlibClasses[1];
        FakeIl2CppImage& unity = g_unityImage;
        FakeIl2CppClass* valueType = AddClass(unity, g_unityClasses, "System", "ValueType", object, 0x12);
        FakeIl2CppClass* enumBase = AddClass(unity, g_unityClasses, "System", "Enum", valueType, 0x12);
        g_int32Class = AddClass(unity, g_unityClasses, "System", "Int32", valueType, 0x08, 4);
        g_booleanClass = AddClass(unity, g_unityClasses, "System", "Boolean", valueType, 0x02, 1);
        FakeIl2CppClass* single = AddClass(unity, g_unityClasses, "System", "Single", valueType, 0x0C, 4);
        FakeIl2CppClass* voidClass = AddClass(unity, g_unityClasses, "System", "Void", valueType, 0x01, 1);
        g_stringClass = AddClass(unity, g_unityClasses, "System", "String", object, 0x0E);
        g_typeClass = AddClass(unity, g_unityClasses, "System", "Type", object, 0x12);
        g_nullReference = AddClass(unity, g_unityClasses, "System", "NullReferenceException", object, 0x12);
        g_invalidOperation = AddClass(unity, g_unityClasses, "System", "InvalidOperationException", object, 0x12);
        g_objectClass = AddClass(unity, g_unityClasses, "UnityEngine", "Object", object, 0x12);
        g_gameObjectClass = AddClass(unity, g_unityClasses, "UnityEngine", "GameObject", g_objectClass, 0x12);
        g_componentClass = AddClass(unity, g_unityClasses, "UnityEngine", "Component", g_objectClass, 0x12);
        g_transformClass = AddClass(unity, g_unityClasses, "UnityEngine", "Transform", g_componentClass, 0x12);
        g_behaviourClass = AddClass(unity, g_unityClasses, "UnityEngine", "Behaviour", g_componentClass, 0x12);
        FakeIl2CppClass* monoBehaviour = AddClass(unity, g_unityClasses, "UnityEngine", "MonoBehaviour", g_behaviourClass, 0x12);
        g_vectorClass = AddClass(unity, g_unityClasses, "UnityEngine", "Vector3", valueType, 0x11, sizeof(SceneVector));
        FakeIl2CppClass* application = AddClass(unity, g_unityClasses, "UnityEngine", "Application", object, 0x12);
        FakeIl2CppClass* resources = AddClass(unity, g_unityClasses, "UnityEngine", "Resources", object, 0x12);
        FakeIl2CppClass* synchronization = AddClass(unity, g_unityClasses, "UnityEngine", "UnitySynchronizationContext", object, 0x12);
        FakeIl2CppClass* beforeRender = AddClass(unity, g_unityClasses, "UnityEngine", "BeforeRenderHelper", object, 0x12);
        g_sceneStruct = AddClass(unity, g_unityClasses, "UnityEngine.SceneManagement", "Scene", valueType, 0x11, 4);
        FakeIl2CppClass* sceneManager = AddClass(unity, g_unityClasses, "UnityEngine.SceneManagement", "SceneManager", object, 0x12);
        FakeIl2CppClass* mode = AddClass(g_sceneGameImage, g_sceneGameClasses, "Game", "Mode", enumBase, 0x11, 4, true);
        g_healthClass = AddClass(g_sceneGameImage, g_sceneGameClasses, "Game", "Health", monoBehaviour, 0x12);
        g_gameObjectArray = AddArray(0, g_gameObjectClass, "GameObject[]");
        g_componentArray = AddArray(1, g_componentClass, "Component[]");
        g_objectArray = AddArray(2, g_objectClass, "Object[]");

        const FakeIl2CppType* tInt = TypeOf(g_int32Class);
        const FakeIl2CppType* tBool = TypeOf(g_booleanClass);
        const FakeIl2CppType* tFloat = TypeOf(single);
        const FakeIl2CppType* tVoid = TypeOf(voidClass);
        const FakeIl2CppType* tString = TypeOf(g_stringClass);
        const FakeIl2CppType* tType = TypeOf(g_typeClass);
        const FakeIl2CppType* tObject = TypeOf(g_objectClass);
        const FakeIl2CppType* tGameObject = TypeOf(g_gameObjectClass);
        const FakeIl2CppType* tTransform = TypeOf(g_transformClass);
        const FakeIl2CppType* tVector = TypeOf(g_vectorClass);
        const FakeIl2CppType* tScene = TypeOf(g_sceneStruct);
        const FakeIl2CppType* tMode = TypeOf(mode);
        mode->EnumBase = tInt;
        g_healthClass->StaticData = g_healthStatics;

        AddField(g_int32Class, "m_value", tInt, sizeof(SceneObject));
        AddMethod(g_int32Class, "GetHashCode", tInt, {}, {}, false, +[](void* self, void**, void**) -> void* { return BoxInt(StructInt(self)); });
        AddField(g_vectorClass, "x", tFloat, sizeof(SceneObject));
        AddField(g_vectorClass, "y", tFloat, sizeof(SceneObject) + 4);
        AddField(g_vectorClass, "z", tFloat, sizeof(SceneObject) + 8);
        AddField(g_sceneStruct, "m_Handle", tInt, sizeof(SceneObject));
        AddField(mode, "value__", tInt, sizeof(SceneObject));
        AddField(mode, "Idle", tMode, 0, 0x50, 0);
        AddField(mode, "Fight", tMode, 0, 0x50, 1);
        AddField(mode, "Flee", tMode, 0, 0x50, 2);
        AddField(g_objectClass, "m_CachedPtr", &g_intPtrType, offsetof(UnityObject, CachedPtr));

        g_executeTasks = AddMethod(synchronization, "ExecuteTasks", tVoid, {}, {}, true, nullptr);
        g_executeTasks->MethodPointer = reinterpret_cast<void*>(&SceneExecuteTasks);
        g_beforeRender = AddMethod(beforeRender, "Invoke", tVoid, {}, {}, true, nullptr);
        g_beforeRender->MethodPointer = reinterpret_cast<void*>(&SceneBeforeRender);

        const FakeMethodInfo* getVersion = AddMethod(application, "get_unityVersion", tString, {}, {}, true,
            +[](void*, void**, void**) -> void* { return SceneText(u"2021.3.99f1"); });
        const FakeMethodInfo* getBackground = AddMethod(application, "get_runInBackground", tBool, {}, {}, true,
            +[](void*, void**, void**) -> void* { return BoxBool(g_il2cppRunInBackground != 0); });
        const FakeMethodInfo* setBackground = AddMethod(application, "set_runInBackground", tVoid, { tBool }, { "value" }, true,
            +[](void*, void** params, void**) -> void* { g_il2cppRunInBackground = *static_cast<bool*>(params[0]) ? 1 : 0; return nullptr; });
        AddProperty(application, "unityVersion", getVersion, nullptr);
        AddProperty(application, "runInBackground", getBackground, setBackground);

        AddMethod(sceneManager, "get_sceneCount", tInt, {}, {}, true, +[](void*, void**, void**) -> void* { return BoxInt(2); });
        AddMethod(sceneManager, "GetActiveScene", tScene, {}, {}, true, +[](void*, void**, void**) -> void* { return BoxScene(g_scenes[0].Handle); });
        AddMethod(sceneManager, "GetSceneAt", tScene, { tInt }, { "index" }, true, +[](void*, void** params, void** exc) -> void*
            {
                const int index = *static_cast<int*>(params[0]);
                if (index < 0 || index > 1)
                    return SceneThrow(exc, g_invalidOperation);
                return BoxScene(g_scenes[index].Handle);
            });
        AddMethod(g_sceneStruct, "get_handle", tInt, {}, {}, false, +[](void* self, void**, void**) -> void* { return BoxInt(StructInt(self)); });
        AddMethod(g_sceneStruct, "get_name", tString, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeScene* scene = SceneOf(self, exc);
                return scene ? SceneText(scene->Name) : nullptr;
            });
        AddMethod(g_sceneStruct, "get_buildIndex", tInt, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeScene* scene = SceneOf(self, exc);
                return scene ? BoxInt(scene->BuildIndex) : nullptr;
            });
        AddMethod(g_sceneStruct, "get_rootCount", tInt, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeScene* scene = SceneOf(self, exc);
                return scene ? BoxInt(static_cast<int>(scene->Roots.size())) : nullptr;
            });
        AddMethod(g_sceneStruct, "get_isLoaded", tBool, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                return SceneOf(self, exc) ? BoxBool(true) : nullptr;
            });
        AddMethod(g_sceneStruct, "GetRootGameObjects", TypeOf(g_gameObjectArray), {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeScene* scene = SceneOf(self, exc);
                if (!scene)
                    return nullptr;
                std::vector<void*> roots;
                for (NativeObject* root : scene->Roots)
                    roots.push_back(root->Wrapper);
                return SceneList(g_gameObjectArray, roots);
            });

        const FakeMethodInfo* getName = AddMethod(g_objectClass, "get_name", tString, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? SceneText(native->Name) : nullptr;
            });
        const FakeMethodInfo* setName = AddMethod(g_objectClass, "set_name", tVoid, { tString }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (NativeObject* native = Native(self, exc))
                    native->Name = SceneRead(static_cast<SceneString*>(params[0]));
                return nullptr;
            });
        AddProperty(g_objectClass, "name", getName, setName);
        AddMethod(g_objectClass, "DontDestroyOnLoad", tVoid, { tObject }, { "target" }, true, +[](void*, void** params, void** exc) -> void*
            {
                NativeObject* native = Native(params[0], exc);
                if (!native || native->Parent)
                    return nullptr;
                std::vector<NativeObject*>& from = g_scenes[native->Scene].Roots;
                from.erase(std::remove(from.begin(), from.end(), native), from.end());
                native->Scene = 2;
                g_scenes[2].Roots.push_back(native);
                return nullptr;
            });
        AddMethod(g_objectClass, "DestroyImmediate", tVoid, { tObject }, { "obj" }, true, +[](void*, void** params, void** exc) -> void*
            {
                NativeObject* native = Native(params[0], exc);
                if (!native)
                    return nullptr;
                std::vector<NativeObject*>& from = g_scenes[native->Scene].Roots;
                from.erase(std::remove(from.begin(), from.end(), native), from.end());
                native->Wrapper->CachedPtr = nullptr;
                native->Transform->CachedPtr = nullptr;
                g_unityObjects.erase(std::remove_if(g_unityObjects.begin(), g_unityObjects.end(),
                    [native](UnityObject* candidate) { return candidate == native->Wrapper || candidate == native->Transform; }), g_unityObjects.end());
                ::InterlockedDecrement(&g_il2cppProbesLive);
                return nullptr;
            });
        const auto findAll = [](void** params, bool hidden) -> void*
            {
                const TypeObject* type = static_cast<TypeObject*>(params[0]);
                std::vector<void*> found;
                for (UnityObject* candidate : g_unityObjects)
                {
                    if (type && SceneIsA(candidate->Header.Klass, type->Type->Klass) && candidate->CachedPtr && (hidden || ActiveInHierarchy(candidate->CachedPtr)))
                        found.push_back(candidate);
                }
                return SceneList(g_objectArray, found);
            };
        static void* (*s_findAll)(void**, bool) = findAll;
        AddMethod(g_objectClass, "FindObjectsOfType", TypeOf(g_objectArray), { tType }, { "type" }, true,
            +[](void*, void** params, void**) -> void* { return s_findAll(params, false); });
        AddMethod(resources, "FindObjectsOfTypeAll", TypeOf(g_objectArray), { tType }, { "type" }, true,
            +[](void*, void** params, void**) -> void* { return s_findAll(params, true); });

        AddMethod(g_gameObjectClass, ".ctor", tVoid, { tString }, { "name" }, false, +[](void* self, void** params, void**) -> void*
            {
                UnityObject* wrapper = static_cast<UnityObject*>(self);
                NativeObject* native = new NativeObject();
                native->Name = SceneRead(static_cast<SceneString*>(params[0]));
                native->Wrapper = wrapper;
                wrapper->CachedPtr = native;
                native->Transform = SceneNew<UnityObject>(g_transformClass);
                native->Transform->CachedPtr = native;
                native->Components.push_back(native->Transform);
                g_unityObjects.push_back(wrapper);
                g_unityObjects.push_back(native->Transform);
                g_scenes[0].Roots.push_back(native);
                ::InterlockedIncrement(&g_il2cppProbesMade);
                ::InterlockedIncrement(&g_il2cppProbesLive);
                return nullptr;
            });
        const FakeMethodInfo* getTransform = AddMethod(g_gameObjectClass, "get_transform", tTransform, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? native->Transform : nullptr;
            });
        const FakeMethodInfo* getActiveSelf = AddMethod(g_gameObjectClass, "get_activeSelf", tBool, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxBool(native->Active) : nullptr;
            });
        const FakeMethodInfo* getActiveInHierarchy = AddMethod(g_gameObjectClass, "get_activeInHierarchy", tBool, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxBool(ActiveInHierarchy(native)) : nullptr;
            });
        const FakeMethodInfo* getLayer = AddMethod(g_gameObjectClass, "get_layer", tInt, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxInt(native->Layer) : nullptr;
            });
        const FakeMethodInfo* getTag = AddMethod(g_gameObjectClass, "get_tag", tString, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? SceneText(native->Tag) : nullptr;
            });
        const FakeMethodInfo* getScene = AddMethod(g_gameObjectClass, "get_scene", tScene, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxScene(g_scenes[native->Scene].Handle) : nullptr;
            });
        AddProperty(g_gameObjectClass, "transform", getTransform, nullptr);
        AddProperty(g_gameObjectClass, "activeSelf", getActiveSelf, nullptr);
        AddProperty(g_gameObjectClass, "activeInHierarchy", getActiveInHierarchy, nullptr);
        AddProperty(g_gameObjectClass, "layer", getLayer, nullptr);
        AddProperty(g_gameObjectClass, "tag", getTag, nullptr);
        AddProperty(g_gameObjectClass, "scene", getScene, nullptr);
        AddMethod(g_gameObjectClass, "SetActive", tVoid, { tBool }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (NativeObject* native = Native(self, exc))
                    native->Active = *static_cast<bool*>(params[0]);
                return nullptr;
            });
        AddMethod(g_gameObjectClass, "GetComponents", TypeOf(g_componentArray), { tType }, { "type" }, false,
            +[](void*, void**, void** exc) -> void* { return SceneThrow(exc, g_invalidOperation); }, true);
        AddMethod(g_gameObjectClass, "GetComponents", TypeOf(g_componentArray), { tType }, { "type" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                const TypeObject* type = static_cast<TypeObject*>(params[0]);
                if (!native || !type)
                    return nullptr;
                std::vector<void*> found;
                for (UnityObject* component : native->Components)
                {
                    if (SceneIsA(component->Header.Klass, type->Type->Klass))
                        found.push_back(component);
                }
                return SceneList(g_componentArray, found);
            });

        const FakeMethodInfo* getGameObject = AddMethod(g_componentClass, "get_gameObject", tGameObject, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? native->Wrapper : nullptr;
            });
        AddProperty(g_componentClass, "gameObject", getGameObject, nullptr);

        const FakeMethodInfo* getChildCount = AddMethod(g_transformClass, "get_childCount", tInt, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxInt(static_cast<int>(native->Children.size())) : nullptr;
            });
        AddProperty(g_transformClass, "childCount", getChildCount, nullptr);
        AddMethod(g_transformClass, "GetChild", tTransform, { tInt }, { "index" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                if (!native)
                    return nullptr;
                const int index = *static_cast<int*>(params[0]);
                if (index < 0 || index >= static_cast<int>(native->Children.size()))
                    return SceneThrow(exc, g_invalidOperation);
                return native->Children[static_cast<std::size_t>(index)]->Transform;
            });
        const FakeMethodInfo* getParent = AddMethod(g_transformClass, "get_parent", tTransform, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native && native->Parent ? native->Parent->Transform : nullptr;
            });
        AddProperty(g_transformClass, "parent", getParent, nullptr);
        const FakeMethodInfo* getLocalPosition = AddMethod(g_transformClass, "get_localPosition", tVector, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxVector(native->Position) : nullptr;
            });
        const FakeMethodInfo* setLocalPosition = AddMethod(g_transformClass, "set_localPosition", tVoid, { tVector }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (NativeObject* native = Native(self, exc))
                    native->Position = *static_cast<SceneVector*>(params[0]);
                return nullptr;
            });
        AddProperty(g_transformClass, "localPosition", getLocalPosition, setLocalPosition);
        AddMethod(g_transformClass, "get_localEulerAngles", tVector, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxVector(native->Euler) : nullptr;
            });
        AddMethod(g_transformClass, "set_localEulerAngles", tVoid, { tVector }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (NativeObject* native = Native(self, exc))
                    native->Euler = *static_cast<SceneVector*>(params[0]);
                return nullptr;
            });
        AddMethod(g_transformClass, "get_localScale", tVector, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                return native ? BoxVector(native->Scale) : nullptr;
            });
        AddMethod(g_transformClass, "set_localScale", tVoid, { tVector }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (NativeObject* native = Native(self, exc))
                    native->Scale = *static_cast<SceneVector*>(params[0]);
                return nullptr;
            });
        AddMethod(g_transformClass, "get_position", tVector, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                const NativeObject* native = Native(self, exc);
                if (!native)
                    return nullptr;
                SceneVector world{};
                for (const NativeObject* at = native; at; at = at->Parent)
                {
                    world.X += at->Position.X;
                    world.Y += at->Position.Y;
                    world.Z += at->Position.Z;
                }
                return BoxVector(world);
            });

        const FakeMethodInfo* getEnabled = AddMethod(g_behaviourClass, "get_enabled", tBool, {}, {}, false, +[](void* self, void**, void** exc) -> void*
            {
                return Native(self, exc) ? BoxBool(g_enabled[static_cast<UnityObject*>(self)]) : nullptr;
            });
        const FakeMethodInfo* setEnabled = AddMethod(g_behaviourClass, "set_enabled", tVoid, { tBool }, { "value" }, false, +[](void* self, void** params, void** exc) -> void*
            {
                if (Native(self, exc))
                    g_enabled[static_cast<UnityObject*>(self)] = *static_cast<bool*>(params[0]);
                return nullptr;
            });
        AddProperty(g_behaviourClass, "enabled", getEnabled, setEnabled);

        AddField(g_healthClass, "hp", tInt, offsetof(HealthComponent, Hp));
        AddField(g_healthClass, "regen", tFloat, offsetof(HealthComponent, Regen));
        AddField(g_healthClass, "label", tString, offsetof(HealthComponent, Label));
        AddField(g_healthClass, "target", tGameObject, offsetof(HealthComponent, Target));
        AddField(g_healthClass, "spawn", tVector, offsetof(HealthComponent, Spawn));
        AddField(g_healthClass, "mode", tMode, offsetof(HealthComponent, Mode));
        AddField(g_healthClass, "s_count", tInt, 0, 0x10);
        AddField(g_healthClass, "MaxHp", tInt, 0, 0x50, 999);
        const FakeMethodInfo* getHp = AddMethod(g_healthClass, "get_Hp", tInt, {}, {}, false,
            +[](void* self, void**, void**) -> void* { return BoxInt(static_cast<HealthComponent*>(self)->Hp); });
        const FakeMethodInfo* setHp = AddMethod(g_healthClass, "set_Hp", tVoid, { tInt }, { "value" }, false,
            +[](void* self, void** params, void**) -> void* { static_cast<HealthComponent*>(self)->Hp = *static_cast<int*>(params[0]); return nullptr; });
        AddProperty(g_healthClass, "Hp", getHp, setHp);
        const FakeMethodInfo* getLabel = AddMethod(g_healthClass, "get_Label", tString, {}, {}, false,
            +[](void* self, void**, void**) -> void* { return static_cast<HealthComponent*>(self)->Label; });
        const FakeMethodInfo* setLabel = AddMethod(g_healthClass, "set_Label", tVoid, { tString }, { "value" }, false,
            +[](void* self, void** params, void**) -> void* { static_cast<HealthComponent*>(self)->Label = static_cast<SceneString*>(params[0]); return nullptr; });
        AddProperty(g_healthClass, "Label", getLabel, setLabel);
        const FakeMethodInfo* getTarget = AddMethod(g_healthClass, "get_Target", tGameObject, {}, {}, false,
            +[](void* self, void**, void**) -> void* { return static_cast<HealthComponent*>(self)->Target; });
        const FakeMethodInfo* setTarget = AddMethod(g_healthClass, "set_Target", tVoid, { tGameObject }, { "value" }, false,
            +[](void* self, void** params, void**) -> void* { static_cast<HealthComponent*>(self)->Target = static_cast<UnityObject*>(params[0]); return nullptr; });
        AddProperty(g_healthClass, "Target", getTarget, setTarget);
        const FakeMethodInfo* getBroken = AddMethod(g_healthClass, "get_Broken", tInt, {}, {}, false,
            +[](void*, void**, void** exc) -> void* { return SceneThrow(exc, g_invalidOperation); });
        AddProperty(g_healthClass, "Broken", getBroken, nullptr);
        AddMethod(g_healthClass, "Heal", tInt, { tInt }, { "amount" }, false, +[](void* self, void** params, void**) -> void*
            {
                HealthComponent* health = static_cast<HealthComponent*>(self);
                health->Hp += *static_cast<int*>(params[0]);
                return BoxInt(health->Hp);
            });
        AddMethod(g_healthClass, "Describe", tString, { tString }, { "prefix" }, false, +[](void* self, void** params, void**) -> void*
            {
                const HealthComponent* health = static_cast<HealthComponent*>(self);
                return SceneText(SceneRead(static_cast<SceneString*>(params[0])) + u":" + SceneRead(health->Label));
            });
        AddMethod(g_healthClass, "Explode", tVoid, {}, {}, false, +[](void*, void**, void** exc) -> void* { return SceneThrow(exc, g_invalidOperation); });
        AddMethod(g_healthClass, "TryRead", tBool, { &g_intRefType }, { "value" }, false, +[](void*, void**, void** exc) -> void* { return SceneThrow(exc, g_invalidOperation); });
        AddMethod(g_healthClass, "Count", tInt, {}, {}, true, +[](void*, void**, void**) -> void* { return BoxInt(g_healthStatics[0]); });

        for (auto& [klass, build] : g_builds)
        {
            klass->Methods = build.Methods.data();
            klass->MethodCount = static_cast<unsigned>(build.Methods.size());
            klass->Fields = build.Fields.data();
            klass->FieldCount = static_cast<unsigned>(build.Fields.size());
            klass->Properties = build.Properties.data();
            klass->PropertyCount = static_cast<unsigned>(build.Properties.size());
        }

        g_scenes[0] = { 0x10, u"Main", 0, {} };
        g_scenes[1] = { 0x20, u"Additive", 1, {} };
        g_scenes[2] = { 0x30, u"DontDestroyOnLoad", -1, {} };
        NativeObject* player = AddGameObject(0, nullptr, u"Player");
        player->Tag = u"Player";
        player->Layer = 3;
        player->Position = { 1.0f, 2.0f, 3.0f };
        NativeObject* weapon = AddGameObject(0, player, u"Weapon");
        weapon->Position = { 0.5f, 0.0f, 0.0f };
        AddGameObject(0, weapon, u"Muzzle");
        AddGameObject(0, player, u"Camera");
        NativeObject* enemies = AddGameObject(0, nullptr, u"Enemies");
        enemies->Active = false;
        NativeObject* enemyA = AddGameObject(0, enemies, u"Enemy A");
        AddGameObject(0, enemies, u"Enemy B");
        NativeObject* ui = AddGameObject(0, nullptr, u"UI");
        AddGameObject(0, ui, u"Größe");
        AddGameObject(1, nullptr, u"Props");
        NativeObject* crowd = AddGameObject(1, nullptr, u"Crowd");
        for (int i = 0; i < 3000; ++i)
        {
            const std::string label = "Unit " + std::to_string(i);
            AddGameObject(1, crowd, std::u16string(label.begin(), label.end()).c_str());
        }
        AddGameObject(2, nullptr, u"Services");

        HealthComponent* health = SceneNew<HealthComponent>(g_healthClass);
        health->Base.CachedPtr = player;
        health->Hp = 100;
        health->Regen = 1.5f;
        health->Label = SceneText(u"hero");
        health->Target = enemyA->Wrapper;
        health->Spawn = { 4.0f, 5.0f, 6.0f };
        health->Mode = 1;
        player->Components.push_back(&health->Base);
        g_unityObjects.push_back(&health->Base);
        g_enabled[&health->Base] = true;
    }
}

extern "C"
{
    __declspec(dllexport) const FakeIl2CppType* il2cpp_class_get_type(const FakeIl2CppClass* klass) { return &klass->ByVal; }
    __declspec(dllexport) char* il2cpp_type_get_name(const FakeIl2CppType* type)
    {
        std::string name;
        if (type->Klass)
            name = type->Klass->Namespace[0] ? std::string(type->Klass->Namespace) + "." + type->Klass->Name : std::string(type->Klass->Name);
        else
        {
            switch (type->Type)
            {
            case 0x01: name = "System.Void"; break;
            case 0x02: name = "System.Boolean"; break;
            case 0x08: name = "System.Int32"; break;
            case 0x0C: name = "System.Single"; break;
            case 0x0E: name = "System.String"; break;
            case 0x18: name = "System.IntPtr"; break;
            default: name = "System.Object"; break;
            }
        }
        if (type->ByRef)
            name += "&";
        return ::_strdup(name.c_str());
    }
    __declspec(dllexport) void il2cpp_free(void* memory) { std::free(memory); }
    __declspec(dllexport) const FakeIl2CppClass* il2cpp_class_from_type(const FakeIl2CppType* type) { return type->Klass; }
    __declspec(dllexport) bool il2cpp_class_is_valuetype(const FakeIl2CppClass* klass) { return klass->ValueType; }
    __declspec(dllexport) bool il2cpp_class_is_enum(const FakeIl2CppClass* klass) { return klass->Enum; }
    __declspec(dllexport) const FakeIl2CppType* il2cpp_class_enum_basetype(const FakeIl2CppClass* klass) { return klass->EnumBase; }
    __declspec(dllexport) int il2cpp_class_value_size(const FakeIl2CppClass* klass, unsigned* align)
    {
        if (align)
            *align = 4;
        return klass->ValueSize;
    }
    __declspec(dllexport) int il2cpp_class_get_rank(const FakeIl2CppClass* klass) { return klass->Rank; }
    __declspec(dllexport) const FakeIl2CppClass* il2cpp_class_get_element_class(const FakeIl2CppClass* klass) { return klass->Element; }
    __declspec(dllexport) int il2cpp_class_array_element_size(const FakeIl2CppClass* klass)
    {
        return klass->Element && klass->Element->ValueType ? klass->Element->ValueSize : static_cast<int>(sizeof(void*));
    }
    __declspec(dllexport) const FakePropertyInfo* il2cpp_class_get_properties(const FakeIl2CppClass* klass, void** iter)
    {
        if (!iter || !klass->PropertyCount)
            return nullptr;
        const FakePropertyInfo* property = static_cast<const FakePropertyInfo*>(*iter);
        property = property ? property + 1 : klass->Properties;
        if (property >= klass->Properties + klass->PropertyCount)
            return nullptr;
        *iter = const_cast<void*>(static_cast<const void*>(property));
        return property;
    }
    __declspec(dllexport) const char* il2cpp_property_get_name(const FakePropertyInfo* property) { return property->Name; }
    __declspec(dllexport) const FakeMethodInfo* il2cpp_property_get_get_method(const FakePropertyInfo* property) { return property->Get; }
    __declspec(dllexport) const FakeMethodInfo* il2cpp_property_get_set_method(const FakePropertyInfo* property) { return property->Set; }
    __declspec(dllexport) unsigned il2cpp_method_get_flags(const FakeMethodInfo* method, unsigned* implementation)
    {
        if (implementation)
            *implementation = 0;
        return method->Flags;
    }
    __declspec(dllexport) const FakeIl2CppType* il2cpp_method_get_return_type(const FakeMethodInfo* method) { return static_cast<const FakeIl2CppType*>(method->ReturnType); }
    __declspec(dllexport) const FakeIl2CppType* il2cpp_method_get_param(const FakeMethodInfo* method, unsigned index)
    {
        return method->ParamTypes && index < method->ParamCount ? method->ParamTypes[index] : nullptr;
    }
    __declspec(dllexport) const char* il2cpp_method_get_param_name(const FakeMethodInfo* method, unsigned index)
    {
        return method->ParamNames && index < method->ParamCount ? method->ParamNames[index] : nullptr;
    }
    __declspec(dllexport) bool il2cpp_method_is_generic(const FakeMethodInfo* method) { return method->Generic; }
    __declspec(dllexport) void* il2cpp_class_get_static_field_data(const FakeIl2CppClass* klass) { return klass->StaticData; }
    __declspec(dllexport) void il2cpp_field_static_get_value(const FakeFieldInfo* field, void* value)
    {
        if (field->Flags & 0x40)
        {
            const int literal = static_cast<int>(field->Literal);
            std::memcpy(value, &literal, sizeof(literal));
        }
        else if (field->Parent->StaticData)
            std::memcpy(value, static_cast<char*>(field->Parent->StaticData) + field->Offset, sizeof(int));
    }
    __declspec(dllexport) void* il2cpp_runtime_invoke(const FakeMethodInfo* method, void* object, void** params, void** exc)
    {
        ::InterlockedIncrement(&g_il2cppInvokes);
        SceneTouch();
        if (exc)
            *exc = nullptr;
        if (!method->Invoker || method->Generic)
            return SceneThrow(exc, g_invalidOperation);
        return reinterpret_cast<FakeImpl>(method->Invoker)(object, params, exc);
    }
    __declspec(dllexport) std::uintptr_t il2cpp_gchandle_new(void* object, bool)
    {
        if (!object || !g_sceneClassSet.count(static_cast<SceneObject*>(object)->Klass))
        {
            ::InterlockedIncrement(&g_il2cppHandleErrors);
            return 0;
        }
        std::lock_guard lock(g_handleLock);
        std::size_t index = 0;
        while (index < g_handles.size() && g_handles[index])
            ++index;
        if (index == g_handles.size())
            g_handles.push_back(nullptr);
        g_handles[index] = object;
        ::InterlockedIncrement(&g_il2cppHandlesLive);
        std::uintptr_t handle = index + 1;
        if (sizeof(void*) == 8)
            handle |= static_cast<std::uintptr_t>(0x5Aull << 32);
        return handle;
    }
    __declspec(dllexport) void il2cpp_gchandle_free(std::uintptr_t handle)
    {
        std::lock_guard lock(g_handleLock);
        const bool tagged = sizeof(void*) != 8 || (static_cast<unsigned long long>(handle) >> 32) == 0x5A;
        const std::size_t index = static_cast<std::size_t>(handle & 0xFFFFFFFFu);
        if (!tagged || index == 0 || index > g_handles.size() || !g_handles[index - 1])
        {
            ::InterlockedIncrement(&g_il2cppHandleErrors);
            return;
        }
        g_handles[index - 1] = nullptr;
        ::InterlockedDecrement(&g_il2cppHandlesLive);
    }
    __declspec(dllexport) void* il2cpp_string_new(const char* text)
    {
        SceneTouch();
        std::u16string wide;
        for (const char* at = text; *at; ++at)
            wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(*at)));
        return SceneText(wide);
    }
    __declspec(dllexport) void* il2cpp_type_get_object(const FakeIl2CppType* type)
    {
        SceneTouch();
        TypeObject*& object = g_typeObjects[type];
        if (!object)
        {
            object = SceneNew<TypeObject>(g_typeClass);
            object->Type = type;
        }
        return object;
    }
    __declspec(dllexport) void* il2cpp_object_new(const FakeIl2CppClass* klass)
    {
        SceneTouch();
        return SceneNew<UnityObject>(klass, 64);
    }
    __declspec(dllexport) void* il2cpp_value_box(const FakeIl2CppClass* klass, void* data)
    {
        SceneTouch();
        return SceneBox(klass, data, static_cast<std::size_t>(klass->ValueSize));
    }
    __declspec(dllexport) void il2cpp_field_set_value(void* object, const FakeFieldInfo* field, void* value)
    {
        SceneTouch();
        *reinterpret_cast<void**>(static_cast<char*>(object) + field->Offset) = value;
    }
    __declspec(dllexport) void il2cpp_field_static_set_value(const FakeFieldInfo* field, void* value)
    {
        SceneTouch();
        if (field->Parent->StaticData)
            *reinterpret_cast<void**>(static_cast<char*>(field->Parent->StaticData) + field->Offset) = value;
    }
}
