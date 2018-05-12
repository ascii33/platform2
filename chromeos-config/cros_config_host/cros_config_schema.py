#!/usr/bin/env python2
# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Transforms and validates cros config from source YAML to target JSON"""

from __future__ import print_function

import argparse
import collections
import copy
import json
from jsonschema import validate
import os
import re
import sys
import yaml

this_dir = os.path.dirname(__file__)

CHROMEOS = 'chromeos'
CONFIGS = 'configs'
DEVICES = 'devices'
PRODUCTS = 'products'
SKUS = 'skus'
CONFIG = 'config'
BUILD_ONLY_ELEMENTS = [
    '/firmware', '/firmware-signing', '/audio/main/files', '/touch/files',
    '/arc/files', '/thermal'
]
BRAND_ELEMENTS = ['brand-code', 'firmware-signing', 'wallpaper']
TEMPLATE_PATTERN = re.compile('{{([^}]*)}}')


def GetNamedTuple(mapping):
  """Converts a mapping into Named Tuple recursively.

  Args:
    mapping: A mapping object to be converted.

  Returns:
    A named tuple generated from mapping
  """
  if not isinstance(mapping, collections.Mapping):
    return mapping
  new_mapping = {}
  for k, v in mapping.iteritems():
    if type(v) is list:
      new_list = []
      for val in v:
        new_list.append(GetNamedTuple(val))
      new_mapping[k.replace('-', '_').replace('@', '_')] = new_list
    else:
      new_mapping[k.replace('-', '_').replace('@', '_')] = GetNamedTuple(v)
  return collections.namedtuple('Config', new_mapping.iterkeys())(**new_mapping)


def MergeDictionaries(primary, overlay):
  """Merges the overlay dictionary onto the primary dictionary.

  If an element doesn't exist, it's added.
  If the element is a list, they are appended to each other.
  Otherwise, the overlay value takes precedent.

  Args:
    primary: Primary dictionary
    overlay: Overlay dictionary
  """
  for overlay_key in overlay.keys():
    overlay_value = overlay[overlay_key]
    if not overlay_key in primary:
      primary[overlay_key] = overlay_value
    elif isinstance(overlay_value, collections.Mapping):
      MergeDictionaries(primary[overlay_key], overlay_value)
    elif type(overlay_value) is list:
      primary[overlay_key].extend(overlay_value)
    else:
      primary[overlay_key] = overlay_value


def ParseArgs(argv):
  """Parse the available arguments.

  Invalid arguments or -h cause this function to print a message and exit.

  Args:
    argv: List of string arguments (excluding program name / argv[0])

  Returns:
    argparse.Namespace object containing the attributes.
  """
  parser = argparse.ArgumentParser(
      description='Validates a YAML cros-config and transforms it to JSON')
  parser.add_argument(
      '-s',
      '--schema',
      type=str,
      help='Path to the schema file used to validate the config')
  parser.add_argument(
      '-c',
      '--config',
      type=str,
      help='Path to the YAML config file that will be validated/transformed')
  parser.add_argument(
      '-m',
      '--configs',
      nargs='+',
      type=str,
      help='Path to the YAML config file(s) that will be validated/transformed')
  parser.add_argument(
      '-o',
      '--output',
      type=str,
      help='Output file that will be generated by the transform (system file)')
  parser.add_argument(
      '-g',
      '--generated_c_output',
      type=str,
      help='Output file that will contain generated c bindings of the config.')
  parser.add_argument(
      '-f',
      '--filter',
      type=bool,
      default=False,
      help='Filter build specific elements from the output JSON')
  return parser.parse_args(argv)


def _SetTemplateVars(template_input, template_vars):
  """Builds a map of template variables by walking the input recursively.

  Args:
    template_input: A mapping object to be walked.
    template_vars: A mapping object built up while walking the template_input.
  """
  to_walk = []
  for key, val in template_input.iteritems():
    if isinstance(val, collections.Mapping):
      to_walk.append(val)
    elif type(val) is not list:
      template_vars[key] = val

  # Do this last so all variables from the parent are in scope first.
  for val in to_walk:
    _SetTemplateVars(val, template_vars)


def _GetVarTemplateValue(val, template_input, template_vars):
  """Applies the templating scheme to a single value.

  Args:
    val: The single val to evaluate.
    template_input: Input that will be updated based on the templating schema.
    template_vars: A mapping of all the variables values available.

  Returns:
    The variable value with templating applied.
  """
  for template_var in TEMPLATE_PATTERN.findall(val):
    replace_string = '{{%s}}' % template_var
    if template_var not in template_vars:
      formatted_vars = json.dumps(template_vars, sort_keys=True, indent=2)
      formatted_input = json.dumps(template_input, sort_keys=True, indent=2)
      error_vals = (template_var, val, formatted_input, formatted_vars)
      raise ValidationError("Referenced template variable '%s' doesn't "
                            "exist string '%s'.\nInput:\n %s\nVariables:\n%s" %
                            error_vals)
    var_value = template_vars[template_var]

    # This is an ugly side effect of templating with primitive values.
    # The template is a string, but the target value needs to be int.
    # This is sort of a hack for now, but if the problem gets worse, we
    # can come up with a more scaleable solution.
    #
    # Guessing this problem won't continue though beyond the use of 'sku-id'
    # since that tends to be the only strongly typed value due to its use
    # for identity detection.
    is_int = isinstance(var_value, int)
    if is_int:
      var_value = str(var_value)

    # If the caller only had one value and it was a template variable that
    # was an int, assume the caller wanted the string to be an int.
    if is_int and val == replace_string:
      val = template_vars[template_var]
    else:
      val = val.replace(replace_string, var_value)
  return val


def _ApplyTemplateVars(template_input, template_vars):
  """Evals the input and applies the templating schema using the provided vars.

  Args:
    template_input: Input that will be updated based on the templating schema.
    template_vars: A mapping of all the variables values available.
  """
  to_walk = []
  for key in template_input.keys():
    val = template_input[key]
    if isinstance(val, collections.Mapping):
      to_walk.append(val)
    elif isinstance(val, list):
      index = 0
      for list_val in val:
        if isinstance(list_val, collections.Mapping):
          to_walk.append(list_val)
        elif isinstance(list_val, basestring):
          val[index] = _GetVarTemplateValue(list_val, template_input,
                                            template_vars)
        index += 1
    elif isinstance(val, basestring):
      template_input[key] = _GetVarTemplateValue(val, template_input,
                                                 template_vars)

  # Do this last so all variables from the parent are in scope first.
  for value in to_walk:
    _ApplyTemplateVars(value, template_vars)


def _DeleteTemplateOnlyVars(template_input):
  """Deletes all variables starting with $

  Args:
    template_input: Input that will be updated based on the templating schema.
  """
  to_delete = []
  for key in template_input.keys():
    val = template_input[key]
    if isinstance(val, collections.Mapping):
      _DeleteTemplateOnlyVars(val)
    elif key.startswith('$'):
      to_delete.append(key)

  for key in to_delete:
    del template_input[key]


def _HasTemplateVariables(template_vars):
  """Checks if there are any unevaluated template variables.

  Args:
    template_vars: A mapping of all the variables values available.

  Returns:
    True if they are still unevaluated template variables.
  """
  for val in template_vars.values():
    if isinstance(val, basestring) and len(TEMPLATE_PATTERN.findall(val)) > 0:
      return True


def TransformConfig(config):
  """Transforms the source config (YAML) to the target system format (JSON)

  Applies consistent transforms to covert a source YAML configuration into
  JSON output that will be used on the system by cros_config.

  Args:
    config: Config that will be transformed.

  Returns:
    Resulting JSON output from the transform.
  """
  config_yaml = yaml.load(config)
  json_from_yaml = json.dumps(config_yaml, sort_keys=True, indent=2)
  json_config = json.loads(json_from_yaml)
  configs = []
  if DEVICES in json_config[CHROMEOS]:
    for device in json_config[CHROMEOS][DEVICES]:
      template_vars = {}
      for product in device.get(PRODUCTS, [{}]):
        for sku in device[SKUS]:
          # Template variables scope is config, then device, then product
          # This allows shared configs to define defaults using anchors, which
          # can then be easily overridden by the product/device scope.
          _SetTemplateVars(sku, template_vars)
          _SetTemplateVars(device, template_vars)
          _SetTemplateVars(product, template_vars)
          while _HasTemplateVariables(template_vars):
            _ApplyTemplateVars(template_vars, template_vars)
          sku_clone = copy.deepcopy(sku)
          _ApplyTemplateVars(sku_clone, template_vars)
          config = sku_clone[CONFIG]
          _DeleteTemplateOnlyVars(config)
          configs.append(config)
  else:
    configs = json_config[CHROMEOS][CONFIGS]

  # Drop everything except for configs since they were just used as shared
  # config in the source yaml.
  json_config = {CHROMEOS: {CONFIGS: configs}}

  return _FormatJson(json_config)


def GenerateCBindings(config):
  """Generates C struct bindings

  Generates C struct bindings that can be used by mosys.

  Args:
    config: Config (transformed) that is the transform basis.
  """
  struct_format_x86 = '''
    {.platform_name = "%s",
     .smbios_match_name = "%s",
     .sku_id = %s,
     .customization_id = "%s",
     .whitelabel_tag = "%s",
     .info = {.brand = "%s",
              .model = "%s",
              .customization = "%s",
              .signature_id = "%s"}}'''
  struct_format_arm = '''
    {.platform_name = "%s",
     .device_tree_compatible_match = "%s",
     .customization_id = "%s",
     .whitelabel_tag = "%s",
     .info = {.brand = "%s",
              .model = "%s",
              .customization = "%s",
              .signature_id = "%s"}}'''
  structs = []
  json_config = json.loads(config)
  for config in json_config[CHROMEOS][CONFIGS]:
    identity = config['identity']
    name = config['name']
    whitelabel_tag = identity.get('whitelabel-tag', '')
    customization_id = identity.get('customization-id', '')
    customization = customization_id or whitelabel_tag or name
    signature_id = config.get('firmware-signing', {}).get('signature-id', '')
    signature_id = signature_id or name
    brand_code = config.get('brand-code', '')
    platform_name = identity.get('platform-name', '')
    device_tree_compatible_match = identity.get(
        'device-tree-compatible-match', '')
    if device_tree_compatible_match:
      structs.append(
          struct_format_arm % (platform_name,
                               device_tree_compatible_match,
                               customization_id,
                               whitelabel_tag,
                               brand_code,
                               name,
                               customization,
                               signature_id))
    else:
      structs.append(
          struct_format_x86 % (platform_name,
                               identity.get('smbios-name-match', ''),
                               identity.get('sku-id', -1),
                               customization_id,
                               whitelabel_tag,
                               brand_code,
                               name,
                               customization,
                               signature_id))
  file_format = '''\
#include "lib/cros_config_struct.h"

static struct config_map all_configs[] = {%s
};

const struct config_map *cros_config_get_config_map(int *num_entries) {
  *num_entries = %s;
  return &all_configs[0];
}
'''

  return file_format % (',\n'.join(structs), len(structs))


def _FormatJson(config):
  """Formats JSON for output or printing.

  Args:
    config: Dictionary to be output
  """
  # Work around bug in json dumps that adds trailing spaces with indent set.
  return re.sub(
      ', $',
      ',',
      json.dumps(config, sort_keys=True, indent=2), flags=re.MULTILINE)


def FilterBuildElements(config):
  """Removes build only elements from the schema.

  Removes build only elements from the schema in preparation for the platform.

  Args:
    config: Config (transformed) that will be filtered
  """
  json_config = json.loads(config)
  for config in json_config[CHROMEOS][CONFIGS]:
    _FilterBuildElements(config, '')

  return _FormatJson(json_config)


def _FilterBuildElements(config, path):
  """Recursively checks and removes build only elements.

  Args:
    config: Dict that will be checked.
    path: Path of elements to filter.
  """
  to_delete = []
  for key in config:
    full_path = '%s/%s' % (path, key)
    if full_path in BUILD_ONLY_ELEMENTS:
      to_delete.append(key)
    elif isinstance(config[key], dict):
      _FilterBuildElements(config[key], full_path)
  for key in to_delete:
    config.pop(key)


def ValidateConfigSchema(schema, config):
  """Validates a transformed cros config against the schema specified

  Verifies that the config complies with the schema supplied.

  Args:
    schema: Source schema used to verify the config.
    config: Config (transformed) that will be verified.
  """
  json_config = json.loads(config)
  schema_yaml = yaml.load(schema)
  schema_json_from_yaml = json.dumps(schema_yaml, sort_keys=True, indent=2)
  schema_json = json.loads(schema_json_from_yaml)
  validate(json_config, schema_json)


class ValidationError(Exception):
  """Exception raised for a validation error"""
  pass


def _ValidateUniqueIdentities(json_config):
  """Verifies the identity tuple is globally unique within the config.

  Args:
    json_config: JSON config dictionary
  """
  identities = [str(config['identity'])
                for config in json_config['chromeos']['configs']]
  if len(identities) != len(set(identities)):
    raise ValidationError('Identities are not unique: %s' % identities)

def _ValidateWhitelabelBrandChangesOnly(json_config):
  """Verifies that whitelabel changes are contained to branding information.

  Args:
    json_config: JSON config dictionary
  """
  whitelabels = {}
  for config in json_config['chromeos']['configs']:
    whitelabel_tag = config['identity'].get('whitelabel-tag', None)
    if whitelabel_tag:
      name = '%s - %s' % (config['name'], config['identity'].get('sku-id', 0))
      config_list = whitelabels.get(name, [])

      wl_minus_brand = copy.deepcopy(config)
      wl_minus_brand['identity']['whitelabel-tag'] = ''

      for brand_element in BRAND_ELEMENTS:
        wl_minus_brand[brand_element] = ''

      config_list.append(wl_minus_brand)
      whitelabels[name] = config_list

    # whitelabels now contains a map by device name with all whitelabel
    # configs that have had their branding data stripped.
    for device_name, configs in whitelabels.iteritems():
      base_config = configs[0]
      compare_index = 1
      while compare_index < len(configs):
        compare_config = configs[compare_index]
        compare_index = compare_index + 1
        base_str = str(base_config)
        compare_str = str(compare_config)
        if base_str != compare_str:
          raise ValidationError(
              'Whitelabel configs can only change branding attributes (%s).\n'
              'However, the device %s differs by other attributes.\n'
              'Example 1: %s\n'
              'Example 2: %s' % (device_name,
                                 ', '.join(BRAND_ELEMENTS),
                                 base_str,
                                 compare_str))

def ValidateConfig(config):
  """Validates a transformed cros config for general business rules.

  Performs name uniqueness checks and any other validation that can't be
  easily performed using the schema.

  Args:
    config: Config (transformed) that will be verified.
  """
  json_config = json.loads(config)
  _ValidateUniqueIdentities(json_config)
  _ValidateWhitelabelBrandChangesOnly(json_config)

def _FindImports(config_file, includes):
  """Recursively looks up and finds files to include for yaml.

  Args:
    config_file: Path to the config file for which to apply imports.
    includes: List that is built up through processing the files.
  """
  working_dir = os.path.dirname(config_file)
  with open(config_file, 'r') as config_stream:
    config_lines = config_stream.readlines()
    yaml_import_lines = []
    found_imports = False
    # Parsing out just the imports snippet is required because the YAML
    # isn't valid until the imports are eval'd
    for line in config_lines:
      if re.match("^imports", line):
        found_imports = True
        yaml_import_lines.append(line)
      elif found_imports:
        match = re.match(" *- (.*)", line)
        if match:
          yaml_import_lines.append(line)
        else:
          break

    if yaml_import_lines:
      yaml_import = yaml.load("\n".join(yaml_import_lines))

      for import_file in yaml_import.get("imports", []):
        full_path = os.path.join(working_dir, import_file)
        _FindImports(full_path, includes)
    includes.append(config_file)

def ApplyImports(config_file):
  """Parses the imports statements and applies them to a result config.

  Args:
    config_file: Path to the config file for which to apply imports.

  Returns:
    Raw config with the imports applied.
  """
  import_files = []
  _FindImports(config_file, import_files)

  all_yaml_files = []
  for import_file in import_files:
    with open(import_file, 'r') as yaml_stream:
      all_yaml_files.append(yaml_stream.read())

  return '\n'.join(all_yaml_files)

def MergeConfigs(configs):
  """Evaluates and merges all config files into a single configuration.

  Args:
    configs: List of source config files that will be transformed/merged.

  Returns:
    Final merged JSON result.
  """
  json_files = []
  for yaml_file in configs:
    yaml_with_imports = ApplyImports(yaml_file)
    json_transformed_file = TransformConfig(yaml_with_imports)
    json_files.append(json.loads(json_transformed_file))

  result_json = json_files[0]
  for overlay_json in json_files[1:]:
    for to_merge_config in overlay_json['chromeos']['configs']:
      to_merge_identity = to_merge_config.get('identity', {})
      to_merge_name = to_merge_config.get('name', '')
      matched = False
      # Find all existing configs where there is a full/partial identity
      # match or name match and merge that config into the source.
      # If there are no matches, then append the config.
      for source_config in result_json['chromeos']['configs']:
        identity_match = False
        if to_merge_identity:
          source_identity = source_config['identity']
          identity_match = True
          for identity_key, identity_value in to_merge_identity.iteritems():
            if (identity_key not in source_identity or
                source_identity[identity_key] != identity_value):
              identity_match = False
              break
        elif to_merge_name:
          identity_match = to_merge_name == source_config.get('name', '')

        if identity_match:
          MergeDictionaries(source_config, to_merge_config)
          matched = True

      if not matched:
        result_json['chromeos']['configs'].append(to_merge_config)

  return _FormatJson(result_json)

def Main(schema,
         config,
         output,
         filter_build_details=False,
         gen_c_bindings_output=None,
         configs=None):
  """Transforms and validates a cros config file for use on the system

  Applies consistent transforms to covert a source YAML configuration into
  a JSON file that will be used on the system by cros_config.

  Verifies that the file complies with the schema verification rules and
  performs additional verification checks for config consistency.

  Args:
    schema: Schema file used to verify the config.
    config: Source config file that will be transformed/verified.
    output: Output file that will be generated by the transform.
    filter_build_details: Whether build only details should be filtered or not.
    gen_c_bindings_output: Output file with generated c bindings.
    configs: List of source config files that will be transformed/verified.
  """
  if not schema:
    schema = os.path.join(this_dir, 'cros_config_schema.yaml')

  # TODO(shapiroc): Remove this once we no longer need backwards compatibility
  # for single config parameters.
  if config:
    configs = [config]

  full_json_transform = MergeConfigs(configs)
  json_transform = full_json_transform

  with open(schema, 'r') as schema_stream:
    ValidateConfigSchema(schema_stream.read(), json_transform)
    ValidateConfig(json_transform)
    if filter_build_details:
      json_transform = FilterBuildElements(json_transform)
  if output:
    with open(output, 'w') as output_stream:
      output_stream.write(json_transform)
  else:
    print(json_transform)
  if gen_c_bindings_output:
    with open(gen_c_bindings_output, 'w') as output_stream:
      output_stream.write(GenerateCBindings(full_json_transform))


def main(_argv=None):
  """Main program which parses args and runs

  Args:
    _argv: Intended to be the list of arguments to the program, or None to use
        sys.argv (but at present this is unused)
  """
  args = ParseArgs(sys.argv[1:])
  Main(args.schema, args.config, args.output, args.filter,
       args.generated_c_output, args.configs)


if __name__ == '__main__':
  main()
