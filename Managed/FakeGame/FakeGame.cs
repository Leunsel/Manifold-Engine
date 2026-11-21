using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

// A tiny ".NET game" for the tests of the .NET reader: TestTarget hosts the
// .NET Framework or .NET with this assembly, calls World.Build once and
// World.Tick now and then, and the tests read it from outside through
// ManifoldClr.exe. Every value is fixed so a test can check it, and Build
// prints the addresses of a few objects so the tests can compare what the
// reader resolves with what the runtime itself says.
namespace Manifold.FakeGame
{
    public struct Vector2
    {
        public float X;
        public float Y;
    }

    public enum Team
    {
        Red = 1,
        Blue = 2,
    }

    public class Entity
    {
        public int Id;
        public Vector2 Position;
    }

    public class Item
    {
        public int Type;
        public int Stack;
        public string Name;
    }

    public class Player : Entity
    {
        public int Health;
        public float Speed;
        public double Gold;
        public long Score;
        public bool Alive;
        public Team Side;
        public string Name;
        public Item[] Inventory;
        public List<Item> Bag;

        public class Buff
        {
            public int Kind;
            public int Time;
        }

        public Buff Active;

        // Kept out of line so it has code of its own (the .NET Framework's
        // JIT would fold a call this small into its caller).
        [MethodImpl(MethodImplOptions.NoInlining)]
        public int Hurt(int amount)
        {
            Health -= amount;
            return Health;
        }

        // Never called, so the runtime never compiles it.
        public int NeverCalled(int a, int b)
        {
            return a * b + Health;
        }
    }

    public static class World
    {
        public static Player LocalPlayer;
        public static Player[] Players;
        public static int Seed;
        public static double Gravity;
        public static Vector2 Spawn;
        public static int Ticks;
        public static string Title;

        public static int Build(string argument)
        {
            Player player = new Player();
            player.Id = 7;
            player.Position = new Vector2 { X = 1.5f, Y = -2.25f };
            player.Health = 1234;
            player.Speed = 2.5f;
            player.Gold = 99.75;
            player.Score = 123456789012L;
            player.Alive = true;
            player.Side = Team.Blue;
            player.Name = "Hero";
            player.Inventory = new Item[3];
            player.Inventory[0] = new Item { Type = 11, Stack = 5, Name = "Torch" };
            player.Bag = new List<Item>();
            player.Bag.Add(new Item { Type = 22, Stack = 1, Name = "Sword" });
            player.Active = new Player.Buff { Kind = 3, Time = 600 };
            LocalPlayer = player;
            Players = new Player[] { player };
            Seed = 4242;
            Gravity = 9.81;
            Spawn = new Vector2 { X = 100f, Y = 200f };
            Title = "Fake World";
            Tick(null);
            // Settle the heap before printing addresses: nothing allocates
            // afterwards, so the objects stay where they are.
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Console.WriteLine("clr player 0x" + Address(LocalPlayer).ToString("X"));
            Console.WriteLine("clr inventory 0x" + Address(LocalPlayer.Inventory).ToString("X"));
            Console.WriteLine("clr bag 0x" + Address(LocalPlayer.Bag).ToString("X"));
            Console.WriteLine("clr name 0x" + Address(LocalPlayer.Name).ToString("X"));
            Console.WriteLine("clr ready");
            Console.Out.Flush();
            return 1;
        }

        public static int Tick(string argument)
        {
            Ticks++;
            LocalPlayer.Hurt(0);
            return Ticks;
        }

        // The entry points for a .NET host, which binds to static methods
        // with plain integer arguments.
        public static int BuildCore(int argument)
        {
            return Build(null);
        }

        public static int TickCore(int argument)
        {
            return Tick(null);
        }

        // The object's address from the reference itself (a test-only trick).
#pragma warning disable CS8500
        static unsafe long Address(object value)
        {
            TypedReference reference = __makeref(value);
            return (long)**(IntPtr**)&reference;
        }
#pragma warning restore CS8500
    }
}
