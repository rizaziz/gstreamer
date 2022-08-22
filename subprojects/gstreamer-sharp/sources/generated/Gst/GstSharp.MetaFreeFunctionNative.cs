// This file was generated by the Gtk# code generator.
// Any changes made will be lost if regenerated.

namespace GstSharp {

	using System;
	using System.Runtime.InteropServices;

#region Autogenerated code
	[UnmanagedFunctionPointer (CallingConvention.Cdecl)]
	internal delegate void MetaFreeFunctionNative(IntPtr meta, IntPtr buffer);

	internal class MetaFreeFunctionInvoker {

		MetaFreeFunctionNative native_cb;
		IntPtr __data;
		GLib.DestroyNotify __notify;

		~MetaFreeFunctionInvoker ()
		{
			if (__notify == null)
				return;
			__notify (__data);
		}

		internal MetaFreeFunctionInvoker (MetaFreeFunctionNative native_cb) : this (native_cb, IntPtr.Zero, null) {}

		internal MetaFreeFunctionInvoker (MetaFreeFunctionNative native_cb, IntPtr data) : this (native_cb, data, null) {}

		internal MetaFreeFunctionInvoker (MetaFreeFunctionNative native_cb, IntPtr data, GLib.DestroyNotify notify)
		{
			this.native_cb = native_cb;
			__data = data;
			__notify = notify;
		}

		internal Gst.MetaFreeFunction Handler {
			get {
				return new Gst.MetaFreeFunction(InvokeNative);
			}
		}

		void InvokeNative (Gst.Meta meta, Gst.Buffer buffer)
		{
			IntPtr native_meta = GLib.Marshaller.StructureToPtrAlloc (meta);
			native_cb (native_meta, buffer == null ? IntPtr.Zero : buffer.Handle);
			Marshal.FreeHGlobal (native_meta);
		}
	}

	internal class MetaFreeFunctionWrapper {

		public void NativeCallback (IntPtr meta, IntPtr buffer)
		{
			try {
				managed (Gst.Meta.New (meta), buffer == IntPtr.Zero ? null : (Gst.Buffer) GLib.Opaque.GetOpaque (buffer, typeof (Gst.Buffer), false));
				if (release_on_call)
					gch.Free ();
			} catch (Exception e) {
				GLib.ExceptionManager.RaiseUnhandledException (e, false);
			}
		}

		bool release_on_call = false;
		GCHandle gch;

		public void PersistUntilCalled ()
		{
			release_on_call = true;
			gch = GCHandle.Alloc (this);
		}

		internal MetaFreeFunctionNative NativeDelegate;
		Gst.MetaFreeFunction managed;

		public MetaFreeFunctionWrapper (Gst.MetaFreeFunction managed)
		{
			this.managed = managed;
			if (managed != null)
				NativeDelegate = new MetaFreeFunctionNative (NativeCallback);
		}

		public static Gst.MetaFreeFunction GetManagedDelegate (MetaFreeFunctionNative native)
		{
			if (native == null)
				return null;
			MetaFreeFunctionWrapper wrapper = (MetaFreeFunctionWrapper) native.Target;
			if (wrapper == null)
				return null;
			return wrapper.managed;
		}
	}
#endregion
}