gst_elements_sources = [
  'gstcapsfilter.c',
  'gstclocksync.c',
  'gstconcat.c',
  'gstdataurisrc.c',
  'gstdownloadbuffer.c',
  'gstcoreelementsplugin.c',
  'gstelements_private.c',
  'gstfakesink.c',
  'gstfakesrc.c',
  'gstfdsink.c',
  'gstfdsrc.c',
  'gstfilesrc.c',
  'gstfilesink.c',
  'gstfunnel.c',
  'gstidentity.c',
  'gstinputselector.c',
  'gstmultiqueue.c',
  'gstoutputselector.c',
  'gstqueue2.c',
  'gstqueue.c',
  'gstsparsefile.c',
  'gststreamiddemux.c',
  'gsttee.c',
  'gsttypefindelement.c',
  'gstvalve.c',
]

gst_elements = library('gstcoreelements',
  gst_elements_sources,
  c_args : gst_c_args,
  include_directories : [configinc],
  dependencies : [gst_dep, gst_base_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gst_elements, install_dir : plugins_pkgconfig_install_dir)

plugins += [gst_elements]
