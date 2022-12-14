cuda_sources = [
  'gstcudaloader.c',
  'gstcudacontext.c',
  'gstcudautils.c',
  'gstcudamemory.c',
  'gstcudabufferpool.c',
  'gstcudanvrtc.c',
]

cuda_headers = [
    'cuda-prelude.h',
    'gstcudabufferpool.h',
    'gstcudacontext.h',
    'gstcudaloader.h',
    'gstcudamemory.h',
    'gstcudanvrtc.h',
    'gstcudautils.h',
]

gstcuda_dep = dependency('', required : false)

if host_system not in ['windows', 'linux']
  subdir_done()
endif

cuda_stubinc = include_directories('./stub')
extra_c_args = ['-DGST_USE_UNSTABLE_API']

if gstgl_dep.found()
  extra_c_args += ['-DHAVE_NVCODEC_GST_GL=1']
endif

if gstd3d11_dep.found()
  extra_c_args += ['-DGST_CUDA_HAS_D3D=1', '-DCOBJMACROS']
endif

pkg_name = 'gstreamer-cuda-' + api_version
gstcuda= library('gstcuda-' + api_version,
  cuda_sources,
  c_args : gst_plugins_bad_args + extra_c_args + ['-DGST_USE_UNSTABLE_API', '-DBUILDING_GST_CUDA', '-DG_LOG_DOMAIN="GStreamer-Cuda"'],
  cpp_args : gst_plugins_bad_args,
  include_directories : [configinc, libsinc, cuda_stubinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gstbase_dep, gmodule_dep, gstvideo_dep, gstglproto_dep, gstd3d11_dep]
)

gen_sources = []
library_def = {'lib': gstcuda}
if build_gir
  gir_includes = ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0', 'CudaGst-1.0']
  if gstglproto_dep.found()
    gir_includes += ['GstGL-1.0']
  endif
  cuda_gir = {
    'sources' : files('stub/cuda.h', 'stub/cudaGL.h'),
    'namespace' : 'CudaGst',
    'nsversion' : api_version,
    'identifier_prefix' : 'CU',
    'symbol_prefix' : ['cu', 'cuda'],
    'includes' : [],
    'install' : true,
    'extra_args' : [],
    'dependencies' : [],
  }
  gir = {
    'sources' : cuda_sources + cuda_headers,
    'namespace' : 'GstCuda',
    'nsversion' : api_version,
    'identifier_prefix' : 'Gst',
    'symbol_prefix' : 'gst',
    'export_packages' : pkg_name,
    'includes' : gir_includes,
    'install' : true,
    'extra_args' : gir_init_section + ['-DGST_USE_UNSTABLE_API'],
    'dependencies' : [gstbase_dep, gstvideo_dep, gstglproto_dep],
  }
  if not static_build
    cudagst_gir = gnome.generate_gir(gstcuda, kwargs: cuda_gir)

    gir += {'includes': gir['includes'] + [cudagst_gir[0]]}
    gst_cuda_gir = gnome.generate_gir(gstcuda, kwargs: gir)
    gen_sources += gst_cuda_gir
  endif

  library_def += {'gir': [gir, cuda_gir]}
endif
libraries += [[pkg_name, library_def]]

pkgconfig.generate(gstcuda,
  libraries : [gstbase_dep, gmodule_dep, gstvideo_dep, gstglproto_dep],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : pkg_name,
  description : 'Unstable library to work with CUDA inside GStreamer',
)

install_headers(cuda_headers + ['cuda-gst.h'], subdir : 'gstreamer-1.0/gst/cuda')
gstcuda_dep = declare_dependency(link_with : gstcuda,
  include_directories : [libsinc],
  dependencies : [gstbase_dep, gmodule_dep, gstvideo_dep, gstglproto_dep],
  sources: gen_sources)

meson.override_dependency(pkg_name, gstcuda_dep)
