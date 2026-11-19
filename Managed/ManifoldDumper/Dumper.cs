using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Manifold.Managed
{
    public static class Dumper
    {
        public const int Idle = 0;
        public const int Running = 1;
        public const int Done = 2;
        public const int Failed = 3;

        public static volatile int State;
        public static volatile int Types;
        public static volatile int Cancel;
        public static volatile IntPtr Data;
        public static volatile int Size;

        public static int Begin()
        {
            if (State == Running)
                return State;
            Release();
            Cancel = 0;
            Types = 0;
            State = Running;
            Thread worker = new Thread(Run);
            worker.IsBackground = true;
            worker.Name = "Manifold dumper";
            worker.Start();
            return State;
        }

        public static int Dump()
        {
            if (State == Running)
                return -1;
            Release();
            Cancel = 0;
            Publish(Build());
            return Size;
        }

        public static int DumpToFile(string path)
        {
            byte[] blob = Build();
            File.WriteAllBytes(path, blob);
            return blob.Length;
        }

        public static int Release()
        {
            if (State == Running)
                return State;
            IntPtr data = Data;
            Data = IntPtr.Zero;
            Size = 0;
            if (data != IntPtr.Zero)
                Marshal.FreeHGlobal(data);
            State = Idle;
            return State;
        }

        static void Run()
        {
            try
            {
                Publish(Build());
            }
            catch (Exception)
            {
                State = Failed;
            }
        }

        static void Publish(byte[] blob)
        {
            IntPtr data = Marshal.AllocHGlobal(blob.Length);
            Marshal.Copy(blob, 0, data, blob.Length);
            Data = data;
            Size = blob.Length;
            State = Done;
        }

        static byte[] Build()
        {
            return new DumpWriter(Type.GetType("Mono.Runtime") != null).Write(AppDomain.CurrentDomain.GetAssemblies());
        }
    }

    sealed class DumpWriter
    {
        const uint Magic = 0x4D44464D;
        const uint Version = 2;
        const uint NoString = 0xFFFFFFFF;
        const BindingFlags Members = BindingFlags.DeclaredOnly | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;

        readonly bool m_mono;
        readonly Dictionary<string, uint> m_index = new Dictionary<string, uint>();
        readonly List<string> m_strings = new List<string>();
        readonly Dictionary<Type, MethodInfo> m_layout = new Dictionary<Type, MethodInfo>();
        readonly MemoryStream m_types = new MemoryStream();
        readonly BinaryWriter m_out;
        uint m_count;
        bool m_layoutMissing;

        public DumpWriter(bool mono)
        {
            m_mono = mono;
            m_out = new BinaryWriter(m_types, Encoding.UTF8);
        }

        public byte[] Write(Assembly[] assemblies)
        {
            Assembly self = typeof(Dumper).Assembly;
            foreach (Assembly assembly in assemblies)
            {
                if (assembly == self)
                    continue;
                Type[] found;
                try
                {
                    found = assembly.GetTypes();
                }
                catch (ReflectionTypeLoadException error)
                {
                    found = error.Types;
                }
                catch (Exception)
                {
                    continue;
                }
                if (found == null)
                    continue;
                string image = ImageName(assembly);
                foreach (Type type in found)
                {
                    if (Dumper.Cancel != 0)
                        throw new OperationCanceledException();
                    if (type == null)
                        continue;
                    WriteType(image, type);
                    ++m_count;
                    Dumper.Types = (int)m_count;
                }
            }
            m_out.Flush();

            MemoryStream result = new MemoryStream();
            BinaryWriter writer = new BinaryWriter(result, Encoding.UTF8);
            writer.Write(Magic);
            writer.Write(Version);
            writer.Write((m_mono ? 1u : 0u) | (m_mono && !m_layoutMissing ? 2u : 0u));
            writer.Write((uint)m_strings.Count);
            foreach (string text in m_strings)
            {
                byte[] bytes = Encoding.UTF8.GetBytes(text);
                writer.Write((uint)bytes.Length);
                writer.Write(bytes);
            }
            writer.Write(m_count);
            writer.Flush();
            m_types.WriteTo(result);
            return result.ToArray();
        }

        void WriteType(string image, Type type)
        {
            string space = "";
            string name = type.Name ?? "";
            string parent = null;
            uint token = 0;
            ulong klass = 0;
            uint flags = 0;
            try
            {
                space = type.IsNested ? "" : type.Namespace ?? "";
                flags = (type.IsValueType ? 1u : 0u) | (type.IsEnum ? 2u : 0u) | (type.IsGenericTypeDefinition ? 4u : 0u) | (type.IsInterface ? 8u : 0u) | (type.IsAbstract ? 16u : 0u) | (type.IsNested ? 32u : 0u);
                if (type.BaseType != null)
                    parent = FullName(type.BaseType);
                token = (uint)type.MetadataToken;
                if (m_mono)
                    klass = NativeAddress.From(Marshal.ReadIntPtr(type.TypeHandle.Value));
            }
            catch (Exception)
            {
            }
            m_out.Write(Text(image));
            m_out.Write(Text(space));
            m_out.Write(Text(name));
            m_out.Write(parent != null ? Text(parent) : NoString);
            m_out.Write(token);
            m_out.Write(klass);
            m_out.Write(flags);

            FieldInfo[] fields = Fields(type);
            m_out.Write((uint)fields.Length);
            bool laidOut = false;
            foreach (FieldInfo field in fields)
            {
                string fieldName = "";
                int offset = -1;
                uint fieldFlags = 0;
                uint code = 0;
                try
                {
                    fieldName = field.Name;
                    fieldFlags = (field.IsStatic ? 1u : 0u) | (field.IsLiteral ? 2u : 0u) | (field.IsInitOnly ? 4u : 0u);
                    if (m_mono && !field.IsLiteral)
                    {
                        if (!laidOut)
                        {
                            laidOut = true;
                            LayOut(field);
                        }
                        offset = Marshal.ReadInt32(field.FieldHandle.Value, 3 * IntPtr.Size);
                    }
                    code = TypeCode(field.FieldType);
                }
                catch (Exception)
                {
                }
                m_out.Write(Text(fieldName));
                m_out.Write(offset);
                m_out.Write(fieldFlags);
                m_out.Write(code);
            }

            List<MethodBase> methods = Methods(type);
            m_out.Write((uint)methods.Count);
            foreach (MethodBase method in methods)
            {
                string methodName = "";
                ulong handle = 0;
                uint methodFlags = 0;
                try
                {
                    methodName = method.Name;
                    methodFlags = (method.IsStatic ? 1u : 0u) | (method.IsVirtual ? 2u : 0u) | (method.IsAbstract ? 4u : 0u) | (method.IsConstructor ? 8u : 0u);
                    if (m_mono)
                        handle = NativeAddress.From(method.MethodHandle.Value);
                }
                catch (Exception)
                {
                }
                m_out.Write(Text(methodName));
                m_out.Write(handle);
                m_out.Write(methodFlags);
                m_out.Write(-1);
            }
        }

        static FieldInfo[] Fields(Type type)
        {
            try
            {
                return type.GetFields(Members);
            }
            catch (Exception)
            {
                return new FieldInfo[0];
            }
        }

        static List<MethodBase> Methods(Type type)
        {
            List<MethodBase> methods = new List<MethodBase>();
            try
            {
                methods.AddRange(type.GetConstructors(Members));
            }
            catch (Exception)
            {
            }
            try
            {
                methods.AddRange(type.GetMethods(Members));
            }
            catch (Exception)
            {
            }
            return methods;
        }

        void LayOut(FieldInfo field)
        {
            Type kind = field.GetType();
            MethodInfo layout;
            if (!m_layout.TryGetValue(kind, out layout))
            {
                layout = kind.GetMethod("GetFieldOffset", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null);
                m_layout.Add(kind, layout);
            }
            if (layout == null)
                m_layoutMissing = true;
            else
                layout.Invoke(field, null);
        }

        uint Text(string text)
        {
            uint index;
            if (m_index.TryGetValue(text, out index))
                return index;
            index = (uint)m_strings.Count;
            m_strings.Add(text);
            m_index.Add(text, index);
            return index;
        }

        static string ImageName(Assembly assembly)
        {
            try
            {
                return assembly.GetName().Name ?? "";
            }
            catch (Exception)
            {
                return "";
            }
        }

        static string FullName(Type type)
        {
            return type.FullName ?? type.Name ?? "";
        }

        static uint TypeCode(Type type)
        {
            if (type.IsByRef)
                return 0x10;
            if (type.IsPointer)
                return 0x0F;
            if (type.IsGenericParameter)
                return type.DeclaringMethod != null ? 0x1Eu : 0x13u;
            if (type.IsArray)
                return type.GetArrayRank() == 1 ? 0x1Du : 0x14u;
            if (type.IsGenericType && !type.IsGenericTypeDefinition)
                return 0x15;
            if (type.IsEnum)
                return 0x11;
            switch (System.Type.GetTypeCode(type))
            {
            case System.TypeCode.Boolean: return 0x02;
            case System.TypeCode.Char: return 0x03;
            case System.TypeCode.SByte: return 0x04;
            case System.TypeCode.Byte: return 0x05;
            case System.TypeCode.Int16: return 0x06;
            case System.TypeCode.UInt16: return 0x07;
            case System.TypeCode.Int32: return 0x08;
            case System.TypeCode.UInt32: return 0x09;
            case System.TypeCode.Int64: return 0x0A;
            case System.TypeCode.UInt64: return 0x0B;
            case System.TypeCode.Single: return 0x0C;
            case System.TypeCode.Double: return 0x0D;
            case System.TypeCode.String: return 0x0E;
            }
            if (type == typeof(IntPtr))
                return 0x18;
            if (type == typeof(UIntPtr))
                return 0x19;
            if (type == typeof(object))
                return 0x1C;
            return type.IsValueType ? 0x11u : 0x12u;
        }
    }
}
