"""Compile native SPIR-V and check the descriptor/stage ABI for the future backend.

Needs the pinned DXC and SPIRV-Tools. No game data or Vulkan device required.
Run: python port/tools/test_native_spirv.py
"""
import re
import subprocess

from vulkan_shaders import ROOT, NATIVE_ENTRIES, generate


EXPECTED = {
    'native_vertex': {'Native': 3},
    'native_pixel': {'Native': 3, 'native_texture': 20, 'native_replace_sampler': 49},
    'native_scanout': {'Display': 1, 'native_source': 21, 'screen': 65},
    'native_convert': {'NativeConvert': 4, 'native_inverse': 22, 'convert_base': 24,
                       **{'convert_colour%d' % i: 25 + i for i in range(4)},
                       **{'convert_depth%d' % i: 29 + i for i in range(4)},
                       'native_converted': 66},
    'present_vertex': {},
    'present_pixel': {'present_source': 16, 'present_sampler': 48},
    'native_blit': {'NativeBlit': 5, 'blit_source': 23},
    'native_upload_alpha': {'NativeBlit': 5, 'blit_source': 23, 'native_source': 21},
}


def main():
    out = ROOT / 'build/port/test-native-spirv'
    out.mkdir(parents=True, exist_ok=True)
    header = out / 'gs_spirv.h'
    generate(header)  # spirv-val is mandatory for every module.
    # The Linux renderer does not reference native modules yet. Optimisation
    # must still keep them in the build, ready for the native Vulkan backend.
    obj = out / 'embedded.o'
    subprocess.run(['clang', '-m32', '-O2', '-x', 'c', '-c', '-include', 'stdint.h',
                    str(header), '-o', str(obj)], check=True)
    embedded = obj.read_bytes()
    modules = {}
    for entry, stage in NATIVE_ENTRIES:
        binary = out / ('gs_spirv_%s.spv' % entry)
        assert binary.read_bytes() in embedded, (entry, 'removed by host compiler')
        dis = subprocess.check_output(['spirv-dis', str(binary)], text=True)
        modules[entry] = dis
        bindings = dict((name, int(binding)) for name, binding in
                        re.findall(r'OpDecorate %(\w+) Binding (\d+)', dis))
        assert bindings == EXPECTED[entry], (entry, bindings)
        sets = re.findall(r'OpDecorate %\w+ DescriptorSet (\d+)', dis)
        assert sets == ['0'] * len(bindings), (entry, sets)
        assert len(set(bindings.values())) == len(bindings), entry
        model = {'vs': 'Vertex', 'ps': 'Fragment', 'cs': 'GLCompute'}[stage]
        assert 'OpEntryPoint %s ' % model in dis, entry
        assert '"%s"' % entry in dis, entry
        # Verify the exact validated bytes, rather than just the generated names.
        words = re.findall(r'0x([0-9a-f]{8})u', header.read_text().split(
            'gs_spirv_%s[]' % entry, 1)[1].split('};', 1)[0])
        assert b''.join(int(w, 16).to_bytes(4, 'little') for w in words) == binary.read_bytes()
    pixel = modules['native_pixel']
    for output, index in (('SV_Target0', 0), ('SV_Target1', 1)):
        assert 'OpDecorate %%out_var_%s Location 0' % output in pixel
        assert 'OpDecorate %%out_var_%s Index %d' % (output, index) in pixel
    # Separate DXC invocations must agree on interpolant locations.
    for vertex, pixel, varyings in (
            ('native_vertex', 'native_pixel', ('TEXCOORD0', 'COLOR0')),
            ('present_vertex', 'present_pixel', ('TEXCOORD0',))):
        for location, varying in enumerate(varyings):
            assert 'OpDecorate %%out_var_%s Location %d' % (varying, location) in modules[vertex]
            assert 'OpDecorate %%in_var_%s Location %d' % (varying, location) in modules[pixel]
    print('PASS: eight validated native modules, bindings, dual-source output, stage linkage and embedded bytes')


if __name__ == '__main__':
    main()
