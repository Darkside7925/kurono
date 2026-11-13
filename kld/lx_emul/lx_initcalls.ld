/* give the container access to the built-in upstream initcall sections, the
 * way vmlinux.lds does for the kernel. every level is declared even when this
 * family has no object at that level, so start/end always exist. (satoru) */
SECTIONS
{
  .initcall0.init  : { __initcall0_start  = .; KEEP(*(.initcall0.init));  __initcall0_end  = .; }
  .initcall1.init  : { __initcall1_start  = .; KEEP(*(.initcall1.init));  __initcall1_end  = .; }
  .initcall2.init  : { __initcall2_start  = .; KEEP(*(.initcall2.init));  __initcall2_end  = .; }
  .initcall3.init  : { __initcall3_start  = .; KEEP(*(.initcall3.init));  __initcall3_end  = .; }
  .initcall4.init  : { __initcall4_start  = .; KEEP(*(.initcall4.init));  __initcall4_end  = .; }
  .initcall5.init  : { __initcall5_start  = .; KEEP(*(.initcall5.init));  __initcall5_end  = .; }
  .initcall6.init  : { __initcall6_start  = .; KEEP(*(.initcall6.init));  __initcall6_end  = .; }
  .initcall7.init  : { __initcall7_start  = .; KEEP(*(.initcall7.init));  __initcall7_end  = .; }
  .initcall7s.init : { __initcall7s_start = .; KEEP(*(.initcall7s.init)); __initcall7s_end = .; }
}
INSERT AFTER .data;
