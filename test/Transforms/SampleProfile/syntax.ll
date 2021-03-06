; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/syntax.prof 2>&1 | FileCheck -check-prefix=NO-DEBUG %s
; RUN: not opt < %s -sample-profile -sample-profile-file=missing.prof 2>&1 | FileCheck -check-prefix=MISSING-FILE %s
; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/bad_fn_header.prof 2>&1 | FileCheck -check-prefix=BAD-FN-HEADER %s
; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/bad_sample_line.prof 2>&1 | FileCheck -check-prefix=BAD-SAMPLE-LINE %s
; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/bad_line_values.prof 2>&1 | FileCheck -check-prefix=BAD-LINE-VALUES %s
; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/bad_discriminator_value.prof 2>&1 | FileCheck -check-prefix=BAD-DISCRIMINATOR-VALUE %s
; RUN: not opt < %s -sample-profile -sample-profile-file=%S/Inputs/bad_samples.prof 2>&1 | FileCheck -check-prefix=BAD-SAMPLES %s

define void @empty() {
entry:
  ret void
}
; NO-DEBUG: LLVM ERROR: No debug information found in function empty
; MISSING-FILE: LLVM ERROR: Could not open file missing.prof:
; BAD-FN-HEADER: LLVM ERROR: {{.*}}bad_fn_header.prof:1: Expected 'mangled_name:NUM:NUM', found empty:100:BAD
; BAD-SAMPLE-LINE: LLVM ERROR: {{.*}}bad_sample_line.prof:3: Expected 'NUM[.NUM]: NUM[ mangled_name:NUM]*', found 1: BAD
; BAD-LINE-VALUES: LLVM ERROR: {{.*}}bad_line_values.prof:2: Expected 'mangled_name:NUM:NUM', found -1: 10
; BAD-DISCRIMINATOR-VALUE: LLVM ERROR: {{.*}}bad_discriminator_value.prof:2: Expected 'NUM[.NUM]: NUM[ mangled_name:NUM]*', found 1.-3: 10
; BAD-SAMPLES: LLVM ERROR: {{.*}}bad_samples.prof:2: Expected 'NUM[.NUM]: NUM[ mangled_name:NUM]*', found 1.3: -10
