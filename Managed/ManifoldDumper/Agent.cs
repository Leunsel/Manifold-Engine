using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Manifold.Managed
{
    public static class Agent
    {
        public const int Version = 3;
        public const int Idle = 0;
        public const int Hooked = 1;
        public const int Failed = 2;

        public static volatile IntPtr Mailbox;
        public static object[] Slots;
        public static volatile int State;
        public static string Hook = "";
        public static string LastError = "";

        internal const int Magic = 0x4741464D;
        internal const int OffMagic = 0;
        internal const int OffVersion = 4;
        internal const int OffRequestSeq = 8;
        internal const int OffDoneSeq = 12;
        internal const int OffRequest = 16;
        internal const int OffRequestCapacity = 24;
        internal const int OffRequestSize = 28;
        internal const int OffResponse = 32;
        internal const int OffResponseSize = 40;
        internal const int OffHeartbeat = 44;
        internal const int OffFrames = 48;
        internal const int OffState = 52;
        internal const int OffBusy = 56;
        internal const int ControlSize = 64;
        internal const int RequestCapacity = 1 << 20;
        internal const int SlotCount = 65536;
        const int HeartbeatLimitMs = 30000;
        const long FrameBudgetMs = 4;
        const int StopWaitMs = 100;

        static readonly object s_lock = new object();
        static EventInfo s_event;
        static Delegate s_tick;
        static int s_lastSeq;
        static int s_heartbeat;
        static int s_heartbeatAt;
        static Batch s_pending;
        static IntPtr s_response;
        static int s_responseCapacity;
        static volatile bool s_retire;
        internal static Registry Objects;
        internal static readonly bool IsMono = Type.GetType("Mono.Runtime") != null;

        public static int Begin()
        {
            lock (s_lock)
            {
                try
                {
                    EnsureMailbox();
                    if (State == Hooked)
                        return State;
                    s_retire = false;
                    s_heartbeat = Marshal.ReadInt32(Mailbox, OffHeartbeat);
                    s_heartbeatAt = Environment.TickCount;
                    if (!Attach())
                    {
                        State = Failed;
                        return State;
                    }
                    State = Hooked;
                    Marshal.WriteInt32(Mailbox, OffState, Hooked);
                    return State;
                }
                catch (Exception error)
                {
                    LastError = Describe(error);
                    State = Failed;
                    return State;
                }
            }
        }

        public static int StopNow()
        {
            if (!Monitor.TryEnter(s_lock, StopWaitMs))
            {
                s_retire = true;
                return State;
            }
            try
            {
                Retire();
                return State;
            }
            finally
            {
                Monitor.Exit(s_lock);
            }
        }

        public static int RunScript(string paths)
        {
            try
            {
                string[] parts = paths.Split('|');
                EnsureMailbox();
                byte[] request = File.ReadAllBytes(parts[0]);
                Batch batch = new Batch(0, request);
                batch.Run(long.MaxValue);
                File.WriteAllBytes(parts[1], batch.Result());
                return 1;
            }
            catch (Exception error)
            {
                LastError = Describe(error);
                return -1;
            }
        }

        static void EnsureMailbox()
        {
            if (Objects == null)
            {
                Slots = new object[SlotCount];
                Objects = new Registry(Slots);
            }
            if (Mailbox != IntPtr.Zero)
                return;
            IntPtr box = Marshal.AllocHGlobal(ControlSize);
            for (int i = 0; i < ControlSize; i += 4)
                Marshal.WriteInt32(box, i, 0);
            IntPtr request = Marshal.AllocHGlobal(RequestCapacity);
            Marshal.WriteInt32(box, OffVersion, Version);
            Marshal.WriteIntPtr(box, OffRequest, request);
            Marshal.WriteInt32(box, OffRequestCapacity, RequestCapacity);
            Marshal.WriteInt32(box, OffMagic, Magic);
            Mailbox = box;
        }

        static bool Attach()
        {
            Type application = Unity.Find("UnityEngine.Application");
            EventInfo hook = application != null ? application.GetEvent("onBeforeRender", BindingFlags.Public | BindingFlags.Static) : null;
            string name = "Application.onBeforeRender";
            if (hook == null)
            {
                Type canvas = Unity.Find("UnityEngine.Canvas");
                hook = canvas != null ? canvas.GetEvent("willRenderCanvases", BindingFlags.Public | BindingFlags.Static) : null;
                name = "Canvas.willRenderCanvases";
            }
            if (hook == null)
            {
                LastError = "this game has neither Application.onBeforeRender nor Canvas.willRenderCanvases";
                return false;
            }
            MethodInfo tick = typeof(Agent).GetMethod("Tick", BindingFlags.NonPublic | BindingFlags.Static);
            Delegate callback = Delegate.CreateDelegate(hook.EventHandlerType, tick);
            hook.AddEventHandler(null, callback);
            s_event = hook;
            s_tick = callback;
            Hook = name;
            return true;
        }

        static void Detach()
        {
            if (s_event == null || s_tick == null)
                return;
            try
            {
                s_event.RemoveEventHandler(null, s_tick);
            }
            catch (Exception error)
            {
                LastError = Describe(error);
            }
            s_event = null;
            s_tick = null;
        }

        static void Retire()
        {
            try
            {
                Ops.RestoreRunInBackground();
            }
            catch (Exception error)
            {
                LastError = Describe(error);
            }
            Detach();
            s_pending = null;
            if (Objects != null)
                Objects.Clear();
            Ops.Forget();
            State = Idle;
            if (Mailbox != IntPtr.Zero)
            {
                Marshal.WriteInt32(Mailbox, OffBusy, 0);
                Marshal.WriteInt32(Mailbox, OffState, Idle);
            }
        }

        static void Tick()
        {
            IntPtr box = Mailbox;
            if (box == IntPtr.Zero)
                return;
            lock (s_lock)
            {
                try
                {
                    Marshal.WriteInt32(box, OffFrames, Marshal.ReadInt32(box, OffFrames) + 1);
                    int now = Environment.TickCount;
                    int beat = Marshal.ReadInt32(box, OffHeartbeat);
                    if (beat != s_heartbeat)
                    {
                        s_heartbeat = beat;
                        s_heartbeatAt = now;
                    }
                    else if (now - s_heartbeatAt > HeartbeatLimitMs)
                    {
                        Retire();
                        return;
                    }
                    if (s_retire && s_pending == null)
                    {
                        Retire();
                        return;
                    }
                    if (s_pending == null)
                    {
                        int seq = Marshal.ReadInt32(box, OffRequestSeq);
                        if (seq == s_lastSeq)
                            return;
                        int size = Marshal.ReadInt32(box, OffRequestSize);
                        if (size < 0 || size > RequestCapacity)
                            size = 0;
                        byte[] request = new byte[size];
                        if (size > 0)
                            Marshal.Copy(Marshal.ReadIntPtr(box, OffRequest), request, 0, size);
                        s_pending = new Batch(seq, request);
                        Marshal.WriteInt32(box, OffBusy, 1);
                    }
                    if (!s_pending.Run(Clock.Frequency * FrameBudgetMs / 1000))
                        return;
                    Publish(box, s_pending);
                    s_lastSeq = s_pending.Seq;
                    s_retire = s_retire || s_pending.Retire;
                    s_pending = null;
                    Marshal.WriteInt32(box, OffBusy, 0);
                    if (s_retire)
                        Retire();
                }
                catch (Exception error)
                {
                    LastError = Describe(error);
                }
            }
        }

        static void Publish(IntPtr box, Batch batch)
        {
            byte[] result = batch.Result();
            if (s_responseCapacity < result.Length || s_response == IntPtr.Zero)
            {
                IntPtr old = s_response;
                int capacity = Math.Max(result.Length, 64 * 1024);
                s_response = Marshal.AllocHGlobal(capacity);
                s_responseCapacity = capacity;
                Marshal.WriteIntPtr(box, OffResponse, s_response);
                if (old != IntPtr.Zero)
                    Marshal.FreeHGlobal(old);
            }
            if (result.Length > 0)
                Marshal.Copy(result, 0, s_response, result.Length);
            Marshal.WriteIntPtr(box, OffResponse, s_response);
            Marshal.WriteInt32(box, OffResponseSize, result.Length);
            System.Threading.Thread.MemoryBarrier();
            Marshal.WriteInt32(box, OffDoneSeq, batch.Seq);
        }

        internal static string Describe(Exception error)
        {
            Exception inner = error;
            while (inner is TargetInvocationException && inner.InnerException != null)
                inner = inner.InnerException;
            return inner.GetType().Name + ": " + inner.Message;
        }
    }

    // ∑ Unity strips System.dll down to what the game uses (Stopwatch, Queue<T> and InvalidDataException
    //   are gone in several builds), so the agent uses nothing outside mscorlib.
    static class Clock
    {
        [DllImport("kernel32.dll")]
        static extern bool QueryPerformanceCounter(out long value);

        [DllImport("kernel32.dll")]
        static extern bool QueryPerformanceFrequency(out long value);

        public static readonly long Frequency = ReadFrequency();

        static long ReadFrequency()
        {
            long value;
            return QueryPerformanceFrequency(out value) && value > 0 ? value : 1000;
        }

        public static long Now()
        {
            long value;
            if (QueryPerformanceCounter(out value))
                return value;
            return Environment.TickCount * Frequency / 1000;
        }
    }

    sealed class IntQueue
    {
        int[] m_items = new int[16];
        int m_head;
        int m_count;

        public int Count { get { return m_count; } }

        public void Enqueue(int value)
        {
            if (m_count == m_items.Length)
            {
                int[] grown = new int[m_items.Length * 2];
                for (int i = 0; i < m_count; ++i)
                    grown[i] = m_items[(m_head + i) % m_items.Length];
                m_items = grown;
                m_head = 0;
            }
            m_items[(m_head + m_count) % m_items.Length] = value;
            ++m_count;
        }

        public int Dequeue()
        {
            if (m_count == 0)
                throw new InvalidOperationException("the queue is empty");
            int value = m_items[m_head];
            m_head = (m_head + 1) % m_items.Length;
            --m_count;
            return value;
        }

        public void Clear()
        {
            m_head = 0;
            m_count = 0;
        }
    }

    sealed class RefComparer : IEqualityComparer<object>
    {
        public new bool Equals(object a, object b)
        {
            return ReferenceEquals(a, b);
        }

        public int GetHashCode(object value)
        {
            return RuntimeHelpers.GetHashCode(value);
        }
    }

    sealed class Registry
    {
        public const int IndexBits = 16;
        const int IndexMask = (1 << IndexBits) - 1;
        const int MaxGeneration = 0x7FFF;

        readonly object[] m_slots;
        readonly int[] m_generations;
        readonly Dictionary<object, int> m_index = new Dictionary<object, int>(new RefComparer());
        readonly List<int>[] m_groups;
        readonly IntQueue m_free = new IntQueue();
        int m_next = 1;

        public Registry(object[] slots)
        {
            m_slots = slots;
            m_generations = new int[slots.Length];
            for (int i = 0; i < m_generations.Length; ++i)
                m_generations[i] = 1;
            m_groups = new List<int>[slots.Length];
        }

        static int IndexOf(int id)
        {
            return id & IndexMask;
        }

        int IdOf(int index)
        {
            return (m_generations[index] << IndexBits) | index;
        }

        void Retire(int index)
        {
            m_generations[index] = m_generations[index] >= MaxGeneration ? 1 : m_generations[index] + 1;
        }

        public int Put(object value, int group)
        {
            if (value == null)
                return 0;
            int id;
            if (!m_index.TryGetValue(value, out id))
            {
                int index;
                if (m_next < m_slots.Length)
                    index = m_next++;
                else if (m_free.Count > 0)
                    index = m_free.Dequeue();
                else
                    throw new InvalidOperationException("the explorer holds too many objects; close some inspector tabs");
                m_slots[index] = value;
                m_groups[index] = new List<int>(1);
                id = IdOf(index);
                m_index[value] = id;
            }
            List<int> groups = m_groups[IndexOf(id)];
            if (!groups.Contains(group))
                groups.Add(group);
            return id;
        }

        public object Get(int id)
        {
            if (id <= 0)
                return null;
            int index = IndexOf(id);
            if (index == 0 || index >= m_slots.Length || m_slots[index] == null || IdOf(index) != id)
                throw new InvalidOperationException("the explorer no longer holds object " + id);
            return m_slots[index];
        }

        public List<int> Release(int group)
        {
            List<int> freed = new List<int>();
            for (int index = 1; index < m_next; ++index)
            {
                List<int> groups = m_groups[index];
                if (groups == null || !groups.Remove(group) || groups.Count > 0)
                    continue;
                freed.Add(IdOf(index));
                m_index.Remove(m_slots[index]);
                m_slots[index] = null;
                m_groups[index] = null;
                Retire(index);
                m_free.Enqueue(index);
            }
            return freed;
        }

        public void Clear()
        {
            for (int index = 0; index < m_slots.Length; ++index)
            {
                if (m_slots[index] != null || m_groups[index] != null)
                    Retire(index);
                m_slots[index] = null;
                m_groups[index] = null;
            }
            m_index.Clear();
            m_free.Clear();
            for (int index = 1; index < m_next; ++index)
                m_free.Enqueue(index);
        }
    }

    sealed class Batch
    {
        public readonly int Seq;
        public bool Retire;
        readonly byte[] m_request;
        readonly MemoryStream m_result = new MemoryStream();
        readonly BinaryWriter m_out;
        int m_at;
        int m_left = -1;
        int m_done;

        public Batch(int seq, byte[] request)
        {
            Seq = seq;
            m_request = request;
            m_out = new BinaryWriter(m_result, Encoding.UTF8);
        }

        public bool Run(long budget)
        {
            long started = Clock.Now();
            if (m_left < 0)
            {
                if (m_request.Length < 4)
                {
                    m_left = 0;
                    return true;
                }
                m_left = BitConverter.ToInt32(m_request, 0);
                m_at = 4;
            }
            while (m_left > 0)
            {
                if (m_done > 0 && Clock.Now() - started > budget)
                    return false;
                --m_left;
                if (m_request.Length - m_at < 8)
                {
                    m_left = 0;
                    break;
                }
                int op = BitConverter.ToInt32(m_request, m_at);
                int length = BitConverter.ToInt32(m_request, m_at + 4);
                m_at += 8;
                if (length < 0 || length > m_request.Length - m_at)
                {
                    m_left = 0;
                    break;
                }
                Reader input = new Reader(m_request, m_at, length);
                m_at += length;
                MemoryStream body = new MemoryStream();
                Writer output = new Writer(body);
                int status = 0;
                try
                {
                    Ops.Run(op, input, output, this);
                }
                catch (Exception error)
                {
                    status = 1;
                    body.SetLength(0);
                    output = new Writer(body);
                    output.Text(Agent.Describe(error));
                }
                output.Flush();
                m_out.Write(op);
                m_out.Write(status);
                m_out.Write((int)body.Length);
                body.WriteTo(m_result);
                ++m_done;
            }
            return true;
        }

        public byte[] Result()
        {
            m_out.Flush();
            byte[] body = m_result.ToArray();
            byte[] all = new byte[body.Length + 4];
            BitConverter.GetBytes(m_done).CopyTo(all, 0);
            body.CopyTo(all, 4);
            return all;
        }
    }

    sealed class Reader
    {
        readonly byte[] m_data;
        int m_at;
        readonly int m_end;

        public Reader(byte[] data, int at, int length)
        {
            m_data = data;
            m_at = at;
            m_end = at + length;
        }

        void Need(int bytes)
        {
            if (m_end - m_at < bytes)
                throw new InvalidOperationException("the request is cut short");
        }

        public int I32()
        {
            Need(4);
            int value = BitConverter.ToInt32(m_data, m_at);
            m_at += 4;
            return value;
        }

        public uint U32()
        {
            return (uint)I32();
        }

        public long I64()
        {
            Need(8);
            long value = BitConverter.ToInt64(m_data, m_at);
            m_at += 8;
            return value;
        }

        public float F32()
        {
            Need(4);
            float value = BitConverter.ToSingle(m_data, m_at);
            m_at += 4;
            return value;
        }

        public byte U8()
        {
            Need(1);
            return m_data[m_at++];
        }

        public byte[] Bytes()
        {
            int length = I32();
            if (length < 0)
                throw new InvalidOperationException("a negative length");
            Need(length);
            byte[] value = new byte[length];
            Buffer.BlockCopy(m_data, m_at, value, 0, length);
            m_at += length;
            return value;
        }

        public string Text()
        {
            return Encoding.UTF8.GetString(Bytes());
        }
    }

    sealed class Writer
    {
        readonly BinaryWriter m_out;

        public Writer(Stream stream)
        {
            m_out = new BinaryWriter(stream, Encoding.UTF8);
        }

        public void I32(int value) { m_out.Write(value); }
        public void U32(uint value) { m_out.Write(value); }
        public void I64(long value) { m_out.Write(value); }
        public void F32(float value) { m_out.Write(value); }
        public void U8(int value) { m_out.Write((byte)value); }
        public void Flush() { m_out.Flush(); }

        public void Bytes(byte[] value)
        {
            m_out.Write(value.Length);
            m_out.Write(value);
        }

        public void Text(string value)
        {
            Bytes(Encoding.UTF8.GetBytes(value ?? ""));
        }
    }
}
