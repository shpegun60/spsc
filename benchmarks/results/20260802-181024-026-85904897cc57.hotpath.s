	.file	"hotpath_probe.cpp"
	.text
	.p2align 4
	.globl	spsc_fifo_producer
	.def	spsc_fifo_producer;	.scl	2;	.type	32;	.endef
	.seh_proc	spsc_fifo_producer
spsc_fifo_producer:
.LFB3642:
	.seh_endprologue
	movq	128(%rcx), %rax
	movq	%rax, %r8
	subq	(%rcx), %r8
	cmpq	$1023, %r8
	ja	.L2
.L4:
	movq	128(%rcx), %rax
	andl	$1023, %eax
	movq	%rdx, 256(%rcx,%rax,8)
	movq	128(%rcx), %rax
	addq	$1, %rax
	movq	%rax, 128(%rcx)
	movl	$1, %eax
	ret
	.p2align 4,,10
	.p2align 3
.L2:
	movq	192(%rcx), %r8
	subq	%r8, %rax
	movq	%r8, (%rcx)
	cmpq	$1023, %rax
	jbe	.L4
	xorl	%eax, %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	spsc_fifo_consumer
	.def	spsc_fifo_consumer;	.scl	2;	.type	32;	.endef
	.seh_proc	spsc_fifo_consumer
spsc_fifo_consumer:
.LFB3644:
	.seh_endprologue
	movq	192(%rcx), %r8
	movq	%r8, %rax
	notq	%rax
	addq	64(%rcx), %rax
	cmpq	$1023, %rax
	ja	.L7
.L9:
	movq	192(%rcx), %rax
	andl	$1023, %eax
	movq	256(%rcx,%rax,8), %rax
	addq	%rax, (%rdx)
	movq	192(%rcx), %rax
	addq	$1, %rax
	movq	%rax, 192(%rcx)
	movl	$1, %eax
	ret
	.p2align 4,,10
	.p2align 3
.L7:
	movq	128(%rcx), %rax
	movq	%rax, 64(%rcx)
	subq	%r8, %rax
	subq	$1, %rax
	cmpq	$1023, %rax
	jbe	.L9
	xorl	%eax, %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	spsc_queue_producer
	.def	spsc_queue_producer;	.scl	2;	.type	32;	.endef
	.seh_proc	spsc_queue_producer
spsc_queue_producer:
.LFB3645:
	.seh_endprologue
	cmpb	$0, (%rcx)
	je	.L13
	cmpq	$0, 320(%rcx)
	je	.L13
	movq	192(%rcx), %rax
	movq	%rax, %r8
	subq	64(%rcx), %r8
	cmpq	$1023, %r8
	ja	.L14
.L15:
	movq	320(%rcx), %r8
	movq	192(%rcx), %rax
	andl	$1023, %eax
	leaq	(%r8,%rax,8), %r8
	movq	%rdx, (%r8)
	movq	192(%rcx), %rax
	addq	$1, %rax
	testq	%r8, %r8
	movq	%rax, 192(%rcx)
	setne	%al
	ret
	.p2align 4,,10
	.p2align 3
.L14:
	movq	256(%rcx), %r8
	subq	%r8, %rax
	movq	%r8, 64(%rcx)
	cmpq	$1023, %rax
	jbe	.L15
.L13:
	xorl	%eax, %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	spsc_queue_consumer
	.def	spsc_queue_consumer;	.scl	2;	.type	32;	.endef
	.seh_proc	spsc_queue_consumer
spsc_queue_consumer:
.LFB3646:
	.seh_endprologue
	movq	%rcx, %rax
	movzbl	(%rcx), %ecx
	testb	%cl, %cl
	je	.L19
	cmpq	$0, 320(%rax)
	je	.L19
	movq	256(%rax), %r9
	movq	%r9, %r8
	notq	%r8
	addq	128(%rax), %r8
	cmpq	$1023, %r8
	ja	.L20
.L22:
	movq	256(%rax), %r8
	andl	$1023, %r8d
	movq	320(%rax), %r9
	leaq	(%r9,%r8,8), %r8
	testq	%r8, %r8
	je	.L19
	movq	(%r8), %r8
	addq	%r8, (%rdx)
	movq	256(%rax), %rdx
	addq	$1, %rdx
	movq	%rdx, 256(%rax)
	movl	%ecx, %eax
	ret
	.p2align 4,,10
	.p2align 3
.L20:
	movq	192(%rax), %r8
	movq	%r8, 128(%rax)
	subq	%r9, %r8
	subq	$1, %r8
	cmpq	$1023, %r8
	jbe	.L22
.L19:
	xorl	%ecx, %ecx
	movl	%ecx, %eax
	ret
	.seh_endproc
	.ident	"GCC: (Rev11, Built by MSYS2 project) 15.2.0"
