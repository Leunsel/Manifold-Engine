using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Manifold.Managed
{
    static class Ops
    {
        public const int Info = 1;
        public const int Scenes = 2;
        public const int Roots = 3;
        public const int Children = 4;
        public const int GameObject = 5;
        public const int UnityObject = 6;
        public const int Find = 7;
        public const int Invoke = 8;
        public const int Fields = 9;
        public const int SetField = 10;
        public const int SetActive = 11;
        public const int SetEnabled = 12;
        public const int SetName = 13;
        public const int SetVector = 14;
        public const int RunInBackground = 15;
        public const int Release = 16;
        public const int Native = 17;
        public const int Stop = 18;
        public const int Elements = 19;
        public const int Hold = 20;

        const int ArgNull = 0;
        const int ArgSlot = 1;
        const int ArgBytes = 2;
        const int ArgText = 3;
        const int ValueNone = 0;
        const int ValueBytes = 1;
        const int ValueText = 2;
        const int ValueObject = 3;

        delegate IntPtr Native4(IntPtr a0, IntPtr a1, IntPtr a2, IntPtr a3);

        static readonly Dictionary<long, Native4> s_natives = new Dictionary<long, Native4>();
        static object s_dontDestroy;
        static int s_originalRunInBackground = -1;

        public static void Forget()
        {
            s_dontDestroy = null;
        }

        public static void RestoreRunInBackground()
        {
            if (s_originalRunInBackground < 0)
                return;
            Unity.SetStatic("UnityEngine.Application", "runInBackground", s_originalRunInBackground != 0);
            s_originalRunInBackground = -1;
        }

        static Registry Objects
        {
            get { return Agent.Objects; }
        }

        public static void Run(int op, Reader input, Writer output, Batch batch)
        {
            switch (op)
            {
            case Info:
                output.Text(Unity.GetStatic("UnityEngine.Application", "unityVersion") as string);
                output.I32(ReadRunInBackground());
                output.Text(Agent.Hook);
                return;
            case Scenes:
                ListScenes(input.I32(), output);
                return;
            case Roots:
                ListRoots(input.I32(), input.I32(), input.I32(), output);
                return;
            case Children:
                ListChildren(input.I32(), input.I32(), input.I32(), output);
                return;
            case GameObject:
                DescribeGameObject(input.I32(), input.I32(), input.I32(), output);
                return;
            case UnityObject:
                DescribeObject(input.I32(), input.I32(), output);
                return;
            case Find:
                FindObjects(input, output);
                return;
            case Invoke:
                InvokeMethod(input, output);
                return;
            case Fields:
                ReadFields(input, output);
                return;
            case SetField:
                WriteField(input);
                return;
            case SetActive:
            {
                object target = Objects.Get(input.I32());
                Unity.Call("UnityEngine.GameObject", "SetActive", target, new Type[] { typeof(bool) }, input.U8() != 0);
                return;
            }
            case SetEnabled:
            {
                object target = Objects.Get(input.I32());
                Unity.Set("UnityEngine.Behaviour", "enabled", target, input.U8() != 0);
                return;
            }
            case SetName:
            {
                object target = Objects.Get(input.I32());
                Unity.Set("UnityEngine.Object", "name", target, input.Text());
                return;
            }
            case SetVector:
                WriteVector(input);
                return;
            case RunInBackground:
                SetRunInBackground(input.I32(), output);
                return;
            case Release:
            {
                List<int> freed = Objects.Release(input.I32());
                output.I32(freed.Count);
                foreach (int slot in freed)
                    output.I32(slot);
                return;
            }
            case Native:
                CallNative(input, output);
                return;
            case Stop:
                batch.Retire = true;
                return;
            case Elements:
                ListElements(input.I32(), input.I32(), input.I32(), input.I32(), output);
                return;
            case Hold:
            {
                int group = input.I32();
                int count = input.I32();
                for (int i = 0; i < count; ++i)
                    Objects.Put(Objects.Get(input.I32()), group);
                return;
            }
            }
            throw new NotSupportedException("unknown request " + op);
        }

        static int ReadRunInBackground()
        {
            object value = Unity.GetStatic("UnityEngine.Application", "runInBackground");
            return value is bool ? ((bool)value ? 1 : 0) : -1;
        }

        static void SetRunInBackground(int wanted, Writer output)
        {
            if (wanted >= 0)
            {
                int now = ReadRunInBackground();
                if (s_originalRunInBackground < 0)
                    s_originalRunInBackground = now;
                Unity.SetStatic("UnityEngine.Application", "runInBackground", wanted != 0);
                if (s_originalRunInBackground == wanted)
                    s_originalRunInBackground = -1;
            }
            output.I32(ReadRunInBackground());
            output.U8(s_originalRunInBackground >= 0 ? 1 : 0);
        }

        static int SceneHandle(object scene)
        {
            object handle = Unity.TryGet(scene, "handle");
            if (handle is int)
                return (int)handle;
            FieldInfo field = scene.GetType().GetField("m_Handle", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            return field != null ? (int)field.GetValue(scene) : 0;
        }

        static void ListScenes(int group, Writer output)
        {
            const string manager = "UnityEngine.SceneManagement.SceneManager";
            int count = (int)Unity.GetStatic(manager, "sceneCount");
            object active = Unity.CallStatic(manager, "GetActiveScene", Type.EmptyTypes);
            output.I32(active != null ? SceneHandle(active) : 0);
            count = Math.Min(Math.Max(count, 0), 256);
            output.I32(count);
            for (int i = 0; i < count; ++i)
            {
                object scene = Unity.CallStatic(manager, "GetSceneAt", new Type[] { typeof(int) }, i);
                output.I32(Objects.Put(scene, group));
                output.I32(SceneHandle(scene));
                output.Text(Unity.Get("UnityEngine.SceneManagement.Scene", "name", scene) as string);
                output.I32(ToInt(Unity.Get("UnityEngine.SceneManagement.Scene", "buildIndex", scene), -1));
                output.I32(ToInt(Unity.Get("UnityEngine.SceneManagement.Scene", "rootCount", scene), -1));
                output.U8(true.Equals(Unity.Get("UnityEngine.SceneManagement.Scene", "isLoaded", scene)) ? 1 : 0);
            }
        }

        static object DontDestroyScene()
        {
            if (s_dontDestroy != null)
                return s_dontDestroy;
            Type gameObject = Unity.Need("UnityEngine.GameObject");
            object probe = Activator.CreateInstance(gameObject, new object[] { "Manifold probe" });
            try
            {
                Unity.CallStatic("UnityEngine.Object", "DontDestroyOnLoad", new Type[] { Unity.Need("UnityEngine.Object") }, probe);
                s_dontDestroy = Unity.Get("UnityEngine.GameObject", "scene", probe);
            }
            finally
            {
                Unity.CallStatic("UnityEngine.Object", "DestroyImmediate", new Type[] { Unity.Need("UnityEngine.Object") }, probe);
            }
            return s_dontDestroy;
        }

        static void WriteNode(object gameObject, object transform, int group, Writer output)
        {
            if (transform == null)
                transform = Unity.Get("UnityEngine.GameObject", "transform", gameObject);
            output.I32(Objects.Put(gameObject, group));
            output.I32(Objects.Put(transform, group));
            output.Text(Unity.Get("UnityEngine.Object", "name", gameObject) as string);
            output.U8(true.Equals(Unity.Get("UnityEngine.GameObject", "activeSelf", gameObject)) ? 1 : 0);
            output.I32(transform != null ? ToInt(Unity.Get("UnityEngine.Transform", "childCount", transform), 0) : 0);
        }

        static void ListRoots(int group, int sceneSlot, int max, Writer output)
        {
            object scene = sceneSlot > 0 ? Objects.Get(sceneSlot) : DontDestroyScene();
            output.I32(Objects.Put(scene, group));
            output.I32(SceneHandle(scene));
            Array roots = Unity.Call("UnityEngine.SceneManagement.Scene", "GetRootGameObjects", scene, Type.EmptyTypes) as Array;
            int total = roots != null ? roots.Length : 0;
            int shown = Math.Min(total, Math.Max(max, 0));
            output.I32(total);
            output.I32(shown);
            for (int i = 0; i < shown; ++i)
                WriteNode(roots.GetValue(i), null, group, output);
        }

        static void ListChildren(int group, int transformSlot, int max, Writer output)
        {
            object transform = Objects.Get(transformSlot);
            int total = ToInt(Unity.Get("UnityEngine.Transform", "childCount", transform), 0);
            int shown = Math.Min(total, Math.Max(max, 0));
            output.I32(total);
            output.I32(shown);
            for (int i = 0; i < shown; ++i)
            {
                object child = Unity.Call("UnityEngine.Transform", "GetChild", transform, new Type[] { typeof(int) }, i);
                WriteNode(Unity.Get("UnityEngine.Component", "gameObject", child), child, group, output);
            }
        }

        static void WriteVector(object vector, Writer output)
        {
            for (int i = 0; i < 3; ++i)
            {
                object value = vector != null ? vector.GetType().GetField(i == 0 ? "x" : i == 1 ? "y" : "z").GetValue(vector) : null;
                output.F32(value is float ? (float)value : 0.0f);
            }
        }

        static void WriteTransform(object transform, Writer output)
        {
            if (transform == null)
            {
                output.U8(0);
                for (int i = 0; i < 12; ++i)
                    output.F32(0.0f);
                return;
            }
            output.U8(1);
            foreach (string name in new string[] { "localPosition", "localEulerAngles", "localScale", "position" })
                WriteVector(Unity.Get("UnityEngine.Transform", name, transform), output);
        }

        static void DescribeGameObject(int group, int slot, int maxChildren, Writer output)
        {
            const string go = "UnityEngine.GameObject";
            object target = Objects.Get(slot);
            object transform = Unity.Get(go, "transform", target);
            object scene = Unity.Get(go, "scene", target);
            output.Text(Unity.Get("UnityEngine.Object", "name", target) as string);
            output.Text(Unity.TryGet(target, "tag") as string);
            output.I32(ToInt(Unity.Get(go, "layer", target), 0));
            output.U8(true.Equals(Unity.Get(go, "activeSelf", target)) ? 1 : 0);
            output.U8(true.Equals(Unity.Get(go, "activeInHierarchy", target)) ? 1 : 0);
            output.Text(scene != null ? Unity.Get("UnityEngine.SceneManagement.Scene", "name", scene) as string : "");
            output.I32(Objects.Put(transform, group));
            WriteTransform(transform, output);
            object parent = transform != null ? Unity.Get("UnityEngine.Transform", "parent", transform) : null;
            object parentObject = parent != null ? Unity.Get("UnityEngine.Component", "gameObject", parent) : null;
            output.I32(Objects.Put(parentObject, group));
            output.Text(parentObject != null ? Unity.Get("UnityEngine.Object", "name", parentObject) as string : "");
            int children = transform != null ? ToInt(Unity.Get("UnityEngine.Transform", "childCount", transform), 0) : 0;
            int shown = Math.Min(children, Math.Max(maxChildren, 0));
            output.I32(children);
            output.I32(shown);
            for (int i = 0; i < shown; ++i)
            {
                object child = Unity.Call("UnityEngine.Transform", "GetChild", transform, new Type[] { typeof(int) }, i);
                object childObject = Unity.Get("UnityEngine.Component", "gameObject", child);
                output.I32(Objects.Put(childObject, group));
                output.Text(Unity.Get("UnityEngine.Object", "name", childObject) as string);
            }
            Type component = Unity.Need("UnityEngine.Component");
            Type behaviour = Unity.Find("UnityEngine.Behaviour");
            Array components = Unity.Call(go, "GetComponents", target, new Type[] { typeof(Type) }, component) as Array;
            int count = components != null ? Math.Min(components.Length, 512) : 0;
            output.I32(count);
            for (int i = 0; i < count; ++i)
            {
                object entry = components.GetValue(i);
                output.I32(Objects.Put(entry, group));
                bool switchable = entry != null && behaviour != null && behaviour.IsInstanceOfType(entry);
                output.U8(switchable ? 1 : 0);
                output.U8(switchable && true.Equals(Unity.Get("UnityEngine.Behaviour", "enabled", entry)) ? 1 : 0);
            }
        }

        static void DescribeObject(int group, int slot, Writer output)
        {
            object target = Objects.Get(slot);
            output.Text(Unity.Get("UnityEngine.Object", "name", target) as string);
            Type component = Unity.Find("UnityEngine.Component");
            Type behaviour = Unity.Find("UnityEngine.Behaviour");
            Type transform = Unity.Find("UnityEngine.Transform");
            object owner = component != null && component.IsInstanceOfType(target) ? Unity.Get("UnityEngine.Component", "gameObject", target) : null;
            output.I32(Objects.Put(owner, group));
            output.Text(owner != null ? Unity.Get("UnityEngine.Object", "name", owner) as string : "");
            output.U8(behaviour != null && behaviour.IsInstanceOfType(target) ? (true.Equals(Unity.Get("UnityEngine.Behaviour", "enabled", target)) ? 1 : 0) : 2);
            WriteTransform(transform != null && transform.IsInstanceOfType(target) ? target : null, output);
        }

        static void FindObjects(Reader input, Writer output)
        {
            int group = input.I32();
            string image = input.Text();
            uint token = input.U32();
            bool hidden = input.U8() != 0;
            string filter = input.Text();
            int max = input.I32();
            Type type = Resolve.TypeOf(image, token);
            Array found = hidden ? Unity.CallStatic("UnityEngine.Resources", "FindObjectsOfTypeAll", new Type[] { typeof(Type) }, type) as Array
                                 : Unity.CallStatic("UnityEngine.Object", "FindObjectsOfType", new Type[] { typeof(Type) }, type) as Array;
            int total = found != null ? found.Length : 0;
            List<object> hits = new List<object>();
            List<string> names = new List<string>();
            for (int i = 0; i < total && hits.Count < max; ++i)
            {
                object entry = found.GetValue(i);
                if (entry == null)
                    continue;
                string name = Unity.Get("UnityEngine.Object", "name", entry) as string ?? "";
                if (filter.Length > 0 && name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                    continue;
                hits.Add(entry);
                names.Add(name);
            }
            output.I32(total);
            output.I32(hits.Count);
            for (int i = 0; i < hits.Count; ++i)
            {
                output.I32(Objects.Put(hits[i], group));
                output.Text(names[i]);
            }
        }

        static object Argument(Reader input, Type type)
        {
            int kind = input.U8();
            switch (kind)
            {
            case ArgNull:
                return null;
            case ArgSlot:
                return Objects.Get(input.I32());
            case ArgBytes:
                return Values.FromBytes(type, input.Bytes(), 0);
            case ArgText:
                return input.Text();
            }
            throw new NotSupportedException("unknown argument kind " + kind);
        }

        static void WriteValue(object value, Type type, int size, int group, Writer output)
        {
            if (value == null || type == typeof(void))
            {
                output.U8(ValueNone);
                return;
            }
            string text = value as string;
            if (text != null)
            {
                output.U8(ValueText);
                output.I32(Objects.Put(value, group));
                output.Text(text);
                return;
            }
            if (value.GetType().IsValueType)
            {
                output.U8(ValueBytes);
                output.Bytes(Values.ToBytes(value, size));
                return;
            }
            output.U8(ValueObject);
            output.I32(Objects.Put(value, group));
        }

        static void InvokeMethod(Reader input, Writer output)
        {
            int group = input.I32();
            string image = input.Text();
            uint token = input.U32();
            int selfSlot = input.I32();
            int size = input.I32();
            int count = input.I32();
            object self = Objects.Get(selfSlot);
            MethodBase method = Resolve.Method(image, token, self);
            ParameterInfo[] parameters = method.GetParameters();
            if (count != parameters.Length)
                throw new ArgumentException(method.Name + " takes " + parameters.Length + " arguments, got " + count);
            object[] args = new object[count];
            for (int i = 0; i < count; ++i)
                args[i] = Argument(input, parameters[i].ParameterType);
            if (!method.IsStatic && self == null)
                throw new ArgumentException(method.Name + " needs an object");
            object result = method.Invoke(method.IsStatic ? null : self, args);
            MethodInfo info = method as MethodInfo;
            WriteValue(result, info != null ? info.ReturnType : typeof(void), size, group, output);
        }

        static void ReadFields(Reader input, Writer output)
        {
            int group = input.I32();
            int slot = input.I32();
            int count = input.I32();
            object self = Objects.Get(slot);
            for (int i = 0; i < count; ++i)
            {
                string image = input.Text();
                uint token = input.U32();
                int size = input.I32();
                FieldInfo field = Resolve.Field(image, token, self);
                if (!field.IsStatic && self == null)
                    throw new ArgumentException(field.Name + " needs an object");
                WriteValue(field.GetValue(field.IsStatic ? null : self), field.FieldType, size, group, output);
            }
        }

        static void WriteField(Reader input)
        {
            int slot = input.I32();
            string image = input.Text();
            uint token = input.U32();
            object self = Objects.Get(slot);
            FieldInfo field = Resolve.Field(image, token, self);
            if (!field.IsStatic && self == null)
                throw new ArgumentException(field.Name + " needs an object");
            if (field.IsLiteral)
                throw new FieldAccessException(field.Name + " is a constant");
            field.SetValue(field.IsStatic ? null : self, Argument(input, field.FieldType));
        }

        static void WriteVector(Reader input)
        {
            object target = Objects.Get(input.I32());
            int which = input.I32();
            Type vector = Unity.Need("UnityEngine.Vector3");
            object value = Activator.CreateInstance(vector);
            vector.GetField("x").SetValue(value, input.F32());
            vector.GetField("y").SetValue(value, input.F32());
            vector.GetField("z").SetValue(value, input.F32());
            string property = which == 1 ? "localEulerAngles" : which == 2 ? "localScale" : "localPosition";
            Unity.Set("UnityEngine.Transform", property, target, value);
        }

        static void ListElements(int group, int slot, int start, int count, Writer output)
        {
            Array array = Objects.Get(slot) as Array;
            if (array == null)
                throw new ArgumentException("not an array");
            int total = array.Length;
            int from = Math.Max(0, start);
            int shown = Math.Max(0, Math.Min(count, total - from));
            output.I32(total);
            output.I32(shown);
            bool references = !array.GetType().GetElementType().IsValueType;
            for (int i = 0; i < shown; ++i)
                output.I32(references ? Objects.Put(array.GetValue(from + i), group) : 0);
        }

        static void CallNative(Reader input, Writer output)
        {
            long function = input.I64();
            IntPtr a0 = new IntPtr(input.I64());
            IntPtr a1 = new IntPtr(input.I64());
            IntPtr a2 = new IntPtr(input.I64());
            IntPtr a3 = new IntPtr(input.I64());
            if (function == 0)
                throw new ArgumentException("no function");
            Native4 call;
            if (!s_natives.TryGetValue(function, out call))
            {
                call = (Native4)Marshal.GetDelegateForFunctionPointer(new IntPtr(function), typeof(Native4));
                s_natives.Add(function, call);
            }
            output.I64(call(a0, a1, a2, a3).ToInt64());
        }

        static int ToInt(object value, int fallback)
        {
            return value is int ? (int)value : fallback;
        }
    }

    static class Unity
    {
        const BindingFlags Members = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
        static readonly Dictionary<string, Type> s_types = new Dictionary<string, Type>();
        static readonly Dictionary<string, PropertyInfo> s_properties = new Dictionary<string, PropertyInfo>();
        static readonly Dictionary<string, MethodInfo> s_methods = new Dictionary<string, MethodInfo>();

        public static Type Find(string name)
        {
            Type found;
            lock (s_types)
            {
                if (s_types.TryGetValue(name, out found))
                    return found;
            }
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                try
                {
                    found = assembly.GetType(name, false);
                }
                catch (Exception)
                {
                    found = null;
                }
                if (found != null)
                    break;
            }
            if (found != null)
            {
                lock (s_types)
                    s_types[name] = found;
            }
            return found;
        }

        public static Type Need(string name)
        {
            Type type = Find(name);
            if (type == null)
                throw new MissingMemberException("this game has no " + name);
            return type;
        }

        static PropertyInfo Property(string type, string name)
        {
            string key = type + "." + name;
            PropertyInfo found;
            if (s_properties.TryGetValue(key, out found))
                return found;
            found = Need(type).GetProperty(name, Members);
            if (found == null)
                throw new MissingMemberException(type + " has no property " + name);
            s_properties[key] = found;
            return found;
        }

        static MethodInfo Method(string type, string name, Type[] signature)
        {
            string key = type + "." + name + "/" + signature.Length + (signature.Length > 0 ? signature[0].Name : "");
            MethodInfo found;
            if (s_methods.TryGetValue(key, out found))
                return found;
            found = Need(type).GetMethod(name, Members, null, signature, null);
            if (found == null)
                throw new MissingMemberException(type + " has no method " + name);
            s_methods[key] = found;
            return found;
        }

        public static object Get(string type, string name, object target)
        {
            if (target == null)
                return null;
            return Property(type, name).GetValue(target, null);
        }

        public static object TryGet(object target, string name)
        {
            if (target == null)
                return null;
            try
            {
                PropertyInfo property = target.GetType().GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
                return property != null ? property.GetValue(target, null) : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static void Set(string type, string name, object target, object value)
        {
            if (target == null)
                throw new ArgumentNullException("target");
            Property(type, name).SetValue(target, value, null);
        }

        public static object GetStatic(string type, string name)
        {
            return Property(type, name).GetValue(null, null);
        }

        public static void SetStatic(string type, string name, object value)
        {
            Property(type, name).SetValue(null, value, null);
        }

        public static object Call(string type, string name, object target, Type[] signature, params object[] args)
        {
            if (target == null)
                throw new ArgumentNullException("target");
            return Method(type, name, signature).Invoke(target, args);
        }

        public static object CallStatic(string type, string name, Type[] signature, params object[] args)
        {
            return Method(type, name, signature).Invoke(null, args);
        }
    }

    static class Resolve
    {
        static readonly Dictionary<string, Module> s_modules = new Dictionary<string, Module>();

        static Module ModuleOf(string image)
        {
            Module found;
            if (s_modules.TryGetValue(image, out found))
                return found;
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                string name;
                try
                {
                    name = assembly.GetName().Name;
                }
                catch (Exception)
                {
                    continue;
                }
                if (name == image)
                {
                    found = assembly.ManifestModule;
                    break;
                }
            }
            if (found == null)
                throw new MissingMemberException("no loaded assembly is named " + image);
            s_modules[image] = found;
            return found;
        }

        static Type Closed(Type open, object self)
        {
            if (open == null || !open.IsGenericTypeDefinition || self == null)
                return null;
            for (Type type = self.GetType(); type != null; type = type.BaseType)
            {
                if (type.IsGenericType && type.GetGenericTypeDefinition() == open)
                    return type;
            }
            return null;
        }

        public static Type TypeOf(string image, uint token)
        {
            return ModuleOf(image).ResolveType((int)token);
        }

        public static MethodBase Method(string image, uint token, object self)
        {
            MethodBase method = ModuleOf(image).ResolveMethod((int)token);
            Type closed = Closed(method.DeclaringType, self);
            if (closed != null)
                method = MethodBase.GetMethodFromHandle(method.MethodHandle, closed.TypeHandle);
            if (method.ContainsGenericParameters)
                throw new NotSupportedException(method.Name + " is generic and cannot be called without type arguments");
            return method;
        }

        public static FieldInfo Field(string image, uint token, object self)
        {
            FieldInfo field = ModuleOf(image).ResolveField((int)token);
            Type closed = Closed(field.DeclaringType, self);
            if (closed != null)
                field = FieldInfo.GetFieldFromHandle(field.FieldHandle, closed.TypeHandle);
            return field;
        }
    }

    static class Values
    {
        const BindingFlags InstanceFields = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;

        static int Offset(FieldInfo field)
        {
            if (Agent.IsMono)
                return Marshal.ReadInt32(field.FieldHandle.Value, 3 * IntPtr.Size) - 2 * IntPtr.Size;
            return (int)Marshal.OffsetOf(field.DeclaringType, field.Name);
        }

        static int Primitive(Type type)
        {
            switch (Type.GetTypeCode(type))
            {
            case TypeCode.Boolean:
            case TypeCode.SByte:
            case TypeCode.Byte:
                return 1;
            case TypeCode.Char:
            case TypeCode.Int16:
            case TypeCode.UInt16:
                return 2;
            case TypeCode.Int32:
            case TypeCode.UInt32:
            case TypeCode.Single:
                return 4;
            case TypeCode.Int64:
            case TypeCode.UInt64:
            case TypeCode.Double:
                return 8;
            }
            if (type == typeof(IntPtr) || type == typeof(UIntPtr))
                return IntPtr.Size;
            return 0;
        }

        public static byte[] ToBytes(object value, int size)
        {
            byte[] bytes = new byte[Math.Max(size, 0)];
            if (value != null)
                Write(value, value.GetType(), bytes, 0, 0);
            return bytes;
        }

        static void Write(object value, Type type, byte[] to, int at, int depth)
        {
            if (value == null || depth > 8)
                return;
            if (type.IsEnum)
            {
                type = Enum.GetUnderlyingType(type);
                value = Convert.ChangeType(value, type);
            }
            int width = Primitive(type);
            if (width > 0)
            {
                byte[] raw;
                switch (Type.GetTypeCode(type))
                {
                case TypeCode.Boolean: raw = new byte[] { (byte)((bool)value ? 1 : 0) }; break;
                case TypeCode.SByte: raw = new byte[] { (byte)(sbyte)value }; break;
                case TypeCode.Byte: raw = new byte[] { (byte)value }; break;
                case TypeCode.Char: raw = BitConverter.GetBytes((char)value); break;
                case TypeCode.Int16: raw = BitConverter.GetBytes((short)value); break;
                case TypeCode.UInt16: raw = BitConverter.GetBytes((ushort)value); break;
                case TypeCode.Int32: raw = BitConverter.GetBytes((int)value); break;
                case TypeCode.UInt32: raw = BitConverter.GetBytes((uint)value); break;
                case TypeCode.Int64: raw = BitConverter.GetBytes((long)value); break;
                case TypeCode.UInt64: raw = BitConverter.GetBytes((ulong)value); break;
                case TypeCode.Single: raw = BitConverter.GetBytes((float)value); break;
                case TypeCode.Double: raw = BitConverter.GetBytes((double)value); break;
                default:
                    raw = type == typeof(IntPtr) ? BitConverter.GetBytes(((IntPtr)value).ToInt64()) : BitConverter.GetBytes(((UIntPtr)value).ToUInt64());
                    break;
                }
                if (at >= 0 && at + width <= to.Length)
                    Buffer.BlockCopy(raw, 0, to, at, width);
                return;
            }
            if (!type.IsValueType)
                return;
            foreach (FieldInfo field in type.GetFields(InstanceFields))
            {
                if (field.FieldType.IsValueType)
                    Write(field.GetValue(value), field.FieldType, to, at + Offset(field), depth + 1);
            }
        }

        public static object FromBytes(Type type, byte[] from, int at)
        {
            return Read(type, from, at, 0);
        }

        static object Read(Type type, byte[] from, int at, int depth)
        {
            if (type.IsByRef)
                type = type.GetElementType();
            if (type.IsEnum)
                return Enum.ToObject(type, Read(Enum.GetUnderlyingType(type), from, at, depth));
            int width = Primitive(type);
            if (width > 0)
            {
                if (at < 0 || at + width > from.Length)
                    throw new ArgumentException("the value for " + type.Name + " is too short");
                switch (Type.GetTypeCode(type))
                {
                case TypeCode.Boolean: return from[at] != 0;
                case TypeCode.SByte: return (sbyte)from[at];
                case TypeCode.Byte: return from[at];
                case TypeCode.Char: return BitConverter.ToChar(from, at);
                case TypeCode.Int16: return BitConverter.ToInt16(from, at);
                case TypeCode.UInt16: return BitConverter.ToUInt16(from, at);
                case TypeCode.Int32: return BitConverter.ToInt32(from, at);
                case TypeCode.UInt32: return BitConverter.ToUInt32(from, at);
                case TypeCode.Int64: return BitConverter.ToInt64(from, at);
                case TypeCode.UInt64: return BitConverter.ToUInt64(from, at);
                case TypeCode.Single: return BitConverter.ToSingle(from, at);
                case TypeCode.Double: return BitConverter.ToDouble(from, at);
                }
                return type == typeof(IntPtr) ? (object)new IntPtr(BitConverter.ToInt64(from, at)) : new UIntPtr(BitConverter.ToUInt64(from, at));
            }
            if (!type.IsValueType || depth > 8)
                throw new ArgumentException(type.Name + " cannot be built from bytes");
            object box = Activator.CreateInstance(type);
            foreach (FieldInfo field in type.GetFields(InstanceFields))
            {
                if (field.FieldType.IsValueType)
                    field.SetValue(box, Read(field.FieldType, from, at + Offset(field), depth + 1));
            }
            return box;
        }
    }
}
