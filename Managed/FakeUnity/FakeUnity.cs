using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine.SceneManagement;

namespace UnityEngine.Events
{
    public delegate void UnityAction();
}

namespace UnityEngine
{
    public class Object
    {
        public string name { get; set; }

        public static void DontDestroyOnLoad(Object target)
        {
            GameObject owner = target as GameObject;
            if (owner != null)
                World.Move(owner, World.DontDestroyHandle);
        }

        public static void DestroyImmediate(Object target)
        {
            GameObject owner = target as GameObject;
            if (owner != null)
                World.Remove(owner);
        }

        public static Object[] FindObjectsOfType(Type type)
        {
            return World.Find(type, false);
        }
    }

    public sealed class GameObject : Object
    {
        internal readonly List<Component> m_components = new List<Component>();
        internal int m_scene;

        public GameObject(string name)
        {
            this.name = name;
            tag = "Untagged";
            activeSelf = true;
            transform = new Transform(this);
            World.Add(this);
        }

        public Transform transform { get; private set; }
        public bool activeSelf { get; private set; }
        public int layer { get; set; }
        public string tag { get; set; }

        public bool activeInHierarchy
        {
            get { return activeSelf && (transform.parent == null || transform.parent.gameObject.activeInHierarchy); }
        }

        public Scene scene
        {
            get { return new Scene(m_scene); }
        }

        public void SetActive(bool value)
        {
            activeSelf = value;
        }

        public Component[] GetComponents(Type type)
        {
            List<Component> found = new List<Component>();
            foreach (Component component in m_components)
            {
                if (type.IsInstanceOfType(component))
                    found.Add(component);
            }
            return found.ToArray();
        }

        public T AddComponent<T>() where T : Component, new()
        {
            T component = new T();
            component.gameObject = this;
            component.name = name;
            m_components.Add(component);
            return component;
        }
    }

    public class Component : Object
    {
        public GameObject gameObject { get; internal set; }
    }

    public class Behaviour : Component
    {
        public Behaviour()
        {
            enabled = true;
        }

        public bool enabled { get; set; }
    }

    public sealed class Transform : Component
    {
        internal readonly List<Transform> m_children = new List<Transform>();

        public Transform()
        {
        }

        internal Transform(GameObject owner)
        {
            gameObject = owner;
            name = owner.name;
            localScale = new Vector3(1.0f, 1.0f, 1.0f);
            owner.m_components.Add(this);
        }

        public Transform parent { get; private set; }
        public Vector3 localPosition { get; set; }
        public Vector3 localEulerAngles { get; set; }
        public Vector3 localScale { get; set; }

        public Vector3 position
        {
            get
            {
                Vector3 own = localPosition;
                if (parent == null)
                    return own;
                Vector3 above = parent.position;
                return new Vector3(own.x + above.x, own.y + above.y, own.z + above.z);
            }
        }

        public int childCount
        {
            get { return m_children.Count; }
        }

        public Transform GetChild(int index)
        {
            return m_children[index];
        }

        public void SetParent(Transform value)
        {
            if (parent != null)
                parent.m_children.Remove(this);
            parent = value;
            if (value != null)
            {
                value.m_children.Add(this);
                gameObject.m_scene = value.gameObject.m_scene;
            }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vector3
    {
        public float x;
        public float y;
        public float z;

        public Vector3(float x, float y, float z)
        {
            this.x = x;
            this.y = y;
            this.z = z;
        }
    }

    public static class Application
    {
        public static string unityVersion
        {
            get { return "6000.0.0f1-fake"; }
        }

        public static bool runInBackground { get; set; }

        public static event Events.UnityAction onBeforeRender;

        public static void Render()
        {
            Events.UnityAction callback = onBeforeRender;
            if (callback != null)
                callback();
        }
    }

    public static class Resources
    {
        public static Object[] FindObjectsOfTypeAll(Type type)
        {
            return World.Find(type, true);
        }
    }

    static class World
    {
        public const int DontDestroyHandle = -1;
        public static readonly List<GameObject> Objects = new List<GameObject>();
        public static readonly Dictionary<int, string> Scenes = new Dictionary<int, string>();
        public static int Active = 1;

        public static void Add(GameObject created)
        {
            created.m_scene = Active;
            Objects.Add(created);
        }

        public static void Move(GameObject moved, int scene)
        {
            moved.m_scene = scene;
        }

        public static void Remove(GameObject gone)
        {
            Objects.Remove(gone);
            if (gone.transform.parent != null)
                gone.transform.SetParent(null);
        }

        public static Object[] Find(Type type, bool hidden)
        {
            List<Object> found = new List<Object>();
            foreach (GameObject candidate in Objects)
            {
                if (!hidden && !candidate.activeInHierarchy)
                    continue;
                if (type.IsInstanceOfType(candidate))
                    found.Add(candidate);
                foreach (Component component in candidate.m_components)
                {
                    if (type.IsInstanceOfType(component))
                        found.Add(component);
                }
            }
            return found.ToArray();
        }

        public static GameObject[] Roots(int scene)
        {
            List<GameObject> roots = new List<GameObject>();
            foreach (GameObject candidate in Objects)
            {
                if (candidate.m_scene == scene && candidate.transform.parent == null)
                    roots.Add(candidate);
            }
            return roots.ToArray();
        }
    }
}

namespace UnityEngine.SceneManagement
{
    public struct Scene
    {
        internal int m_Handle;

        internal Scene(int handle)
        {
            m_Handle = handle;
        }

        public int handle
        {
            get { return m_Handle; }
        }

        public string name
        {
            get
            {
                string found;
                return World.Scenes.TryGetValue(m_Handle, out found) ? found : (m_Handle == World.DontDestroyHandle ? "DontDestroyOnLoad" : "");
            }
        }

        public int buildIndex
        {
            get { return m_Handle == World.DontDestroyHandle ? -1 : m_Handle - 1; }
        }

        public int rootCount
        {
            get { return World.Roots(m_Handle).Length; }
        }

        public bool isLoaded
        {
            get { return true; }
        }

        public GameObject[] GetRootGameObjects()
        {
            return World.Roots(m_Handle);
        }

        public void GetRootGameObjects(List<GameObject> into)
        {
            into.AddRange(World.Roots(m_Handle));
        }
    }

    public static class SceneManager
    {
        public static int sceneCount
        {
            get { return World.Scenes.Count; }
        }

        public static Scene GetSceneAt(int index)
        {
            List<int> handles = new List<int>(World.Scenes.Keys);
            handles.Sort();
            return new Scene(handles[index]);
        }

        public static Scene GetActiveScene()
        {
            return new Scene(World.Active);
        }
    }
}

namespace Game
{
    public class Player : UnityEngine.Behaviour
    {
        public int health = 100;
        public string title = "hero";
        public UnityEngine.GameObject target;
        public UnityEngine.GameObject[] friends;
        public UnityEngine.Vector3 spawn = new UnityEngine.Vector3(1.0f, 2.0f, 3.0f);

        public Player()
        {
            Speed = 3.5f;
        }

        public float Speed { get; set; }

        public int Heal(int amount)
        {
            health += amount;
            return health;
        }

        public UnityEngine.Vector3 Offset(UnityEngine.Vector3 by)
        {
            spawn = new UnityEngine.Vector3(spawn.x + by.x, spawn.y + by.y, spawn.z + by.z);
            return spawn;
        }

        public string Describe()
        {
            return title + ":" + health;
        }

        public void Explode()
        {
            throw new InvalidOperationException("boom");
        }
    }

    public static class FakeWorld
    {
        public static int Build(string manifest)
        {
            UnityEngine.World.Scenes[1] = "Main";
            UnityEngine.World.Scenes[2] = "Menu";
            UnityEngine.World.Active = 1;
            UnityEngine.GameObject world = new UnityEngine.GameObject("World");
            UnityEngine.GameObject hero = new UnityEngine.GameObject("Player");
            hero.transform.SetParent(world.transform);
            hero.transform.localPosition = new UnityEngine.Vector3(4.0f, 5.0f, 6.0f);
            hero.layer = 8;
            hero.tag = "Player";
            UnityEngine.GameObject camera = new UnityEngine.GameObject("Camera");
            camera.transform.SetParent(world.transform);
            Player player = hero.AddComponent<Player>();
            player.target = camera;
            player.friends = new UnityEngine.GameObject[] { camera, world };
            UnityEngine.GameObject lights = new UnityEngine.GameObject("Lights");
            lights.SetActive(false);
            UnityEngine.World.Active = 2;
            new UnityEngine.GameObject("MenuRoot");
            UnityEngine.World.Active = 1;

            Type type = typeof(Player);
            const BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;
            List<string> lines = new List<string>();
            lines.Add("image=" + type.Assembly.GetName().Name);
            lines.Add("player=" + type.MetadataToken);
            lines.Add("gameobject=" + typeof(UnityEngine.GameObject).MetadataToken);
            lines.Add("heal=" + type.GetMethod("Heal", all).MetadataToken);
            lines.Add("offset=" + type.GetMethod("Offset", all).MetadataToken);
            lines.Add("describe=" + type.GetMethod("Describe", all).MetadataToken);
            lines.Add("explode=" + type.GetMethod("Explode", all).MetadataToken);
            lines.Add("getspeed=" + type.GetProperty("Speed", all).GetGetMethod(true).MetadataToken);
            lines.Add("setspeed=" + type.GetProperty("Speed", all).GetSetMethod(true).MetadataToken);
            lines.Add("health=" + type.GetField("health", all).MetadataToken);
            lines.Add("title=" + type.GetField("title", all).MetadataToken);
            lines.Add("target=" + type.GetField("target", all).MetadataToken);
            lines.Add("friends=" + type.GetField("friends", all).MetadataToken);
            File.WriteAllLines(manifest, lines.ToArray());
            return UnityEngine.World.Objects.Count;
        }

        public static int Render(string unused)
        {
            UnityEngine.Application.Render();
            return 0;
        }
    }
}
