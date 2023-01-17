# VapourSynth-for-Resolve
VapourSynth OFX plugin for Davinci Resolve Fusion which brings open source Python-based image processing into a Resolve pipeline. (currently Windows OS only)

This OFX plugin is the result of my own frustration at the limited toolset provided within Davinci Resolve Fusion to address a plethora of novel motion picture artifacts for color grading and restoration. If you require more nuanced and deeply customizable tools for your work, this tool will hopefully provide a solution.  

In its current form, the tool can support the following: 
* Deinterlacing using QTGMC 
* Chroma subsampling and upsampling
* Color space conversion

Hopefully with the contributions from the VapourSynth community, we can continue to develop this tool until colorists and restoration artists can't live without it. 

*Ensure that you have installed the most recent version of VapourSynth. Make sure to update all your plugins and script with "vsrepo.py upgrade-all" after installing.

Clearing the script field completely means fast passthrough which can be useful if you use keyframes to control filtering for different sections. Ignore the radius setting since 10 is a sane default for just about everything. Note that if you don't reference anything in vs namespace you don't need the import line, that's why it's omitted in some examples.

Example 1 - (simple RGB supporting filters):
clip = source.text.ClipInfo()
clip.set_output()

Example 2 - (QTGMC/YUV input):
import vapoursynth as vs
import havsfunc as havs
clip = source.resize.Bicubic(format=vs.YUV444P16, transfer_in_s='srgb', matrix_s='709', transfer_s='709')
clip = havs.QTGMC(clip, TFF=True) [::2]
clip = clip.resize.Bicubic(format=source.format)
clip.set_output()

Example 3 - (load something completely different for no good reason):
import vapoursynth as vs
source = source.text.ClipInfo()
s2 = vs.core.ffms2.Source('d:/1080p.png')
s2 = s2.resize. Bicubic  (width=source.width, height=source.height, format= source.format)
s2 = s2 * source.num_frames
s2 = s2.text.ClipInfo()
s2.set_output()

Example 4 (limited reordering, shift everything 5 frames back):
clip = source[0]*5 + source[:-5]
clip.set_output() 
