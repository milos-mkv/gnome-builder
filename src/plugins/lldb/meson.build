# LLDB is the debugger on macOS, and it is driven through lldb-dap
if get_option('plugin_lldb') and host_machine.system() == 'darwin'

plugins_sources += files([
  'gbp-lldb-debugger.c',
  'lldb-plugin.c',
])

plugin_lldb_resources = gnome.compile_resources(
  'lldb-resources',
  'lldb.gresource.xml',
  c_name: 'gbp_lldb',
)

plugins_sources += plugin_lldb_resources

endif
