<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Microsoft Visual C++ runtime in the Windows ZIP

Reviewed 2026-11-18. This records the project's reading of the sources quoted
below. It is not legal advice and not legal clearance. Quotes are from the
documents named next to them, as read on that date. Anything marked
**not verified** could not be confirmed from a primary source.

The [component inventory](components.json) record is `msvc-runtime`.

## What the ZIP carries

The Windows ZIP carries in `bin\` the Visual C++ runtime DLLs its binaries
import, next to `dsd-neo.exe`. The 2.12.1 ZIP carried `MSVCP140.dll`,
`VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`. A release may carry only the files
the `msvc-runtime` record lists under `windows_release`: `concrt140.dll`,
`msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`,
`msvcp140_atomic_wait.dll`, `msvcp140_codecvt_ids.dll`, `vccorlib140.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll` and `vcruntime140_threads.dll`.

- `tools/windows/stage_runtime_dlls.ps1` copies them unmodified from the x64
  runtime folder of the toolset that built the release,
  `VC\Redist\MSVC\<version>\x64\Microsoft.VC<toolset>.CRT`, and never from
  System32. It refuses the debug runtime and any runtime DLL the record does
  not list.
- `tools/windows/verify_release_tree.ps1` refuses the same in the staged tree
  and in the unpacked ZIP. It also refuses a tree that carries any of these
  DLLs without the end-user notice below.
- The ZIP carries that notice as
  `licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`, from
  `packaging/windows/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`.

Microsoft does not recommend this way of shipping the runtime, for servicing
reasons. Its [Determine which DLLs to redistribute](https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute?view=msvc-170)
page says: "You may use these libraries for local deployment, by installing
them in the same directory as your application. We don't recommend local
deployment because it makes you responsible for delivering updates to your
deployed applications." Windows Update does not service these copies, so a
fix in the runtime reaches users only with a new dsd-neo release.

## Microsoft's terms

The terms are the "Distributable Code" section of the Microsoft Software
License Terms for the Visual Studio edition whose licence the release relies
on (see [Build basis](#build-basis)), and the Distributable List that section
points to. The license pages at <https://visualstudio.microsoft.com/license-terms/>
embed Word documents. These were read:

| Terms | Page |
| --- | --- |
| Visual Studio Community 2026 (October 1, 2025) | <https://visualstudio.microsoft.com/license-terms/vs2026-ga-community/> |
| Visual Studio Enterprise/Professional 2026 (EULAID VS2026_EntPro_ENU.1033, October 1, 2025) | <https://visualstudio.microsoft.com/license-terms/vs2026-ga-pro-enterprise/> |
| Visual Studio Community 2022 (EULAID VS_Comm_2022_ENU.1033) | <https://visualstudio.microsoft.com/license-terms/vs2022-ga-community/> |
| Visual Studio Enterprise/Professional 2022 (EULAID VS_EntPro_2022_ENU.1033) | <https://visualstudio.microsoft.com/license-terms/vs2022-ga-proenterprise/> |
| Visual Studio 2026 Remote Debugger … and Build Tools (October 1, 2025) | <https://visualstudio.microsoft.com/license-terms/vs2026-ga-diagnostic-buildtools/> |
| Visual C++ v14 Redistributable and Runtime (October 1, 2025) | <https://visualstudio.microsoft.com/license-terms/vs2026-ga-visualcpp-v14-redist-runtime/> |

The documents number their lists with Word's automatic numbering, which does
not survive text extraction. Clauses are cited below by their headings, and
list items in their original order.

### Distributable Code

Visual Studio Enterprise/Professional 2026, verbatim. Visual Studio Community
2026 has the same text, the indemnity item included.

> DISTRIBUTABLE CODE. The software contains code that you may distribute in
> applications you develop as described in this Section. For purposes of this
> Section, the term "distribution" also means deployment of your applications
> for third parties to access over the Internet.
>
> Right to Use and Distribute. The code and text files listed in (i) and (ii)
> below are "Distributable Code".
>
> Distributable List. You may copy and distribute the object code form of code
> listed on the Distributable List located at https://aka.ms/vs/18/redistribution.
>
> […]
>
> Third Party Distribution. You may permit distributors of your applications
> to copy and distribute the Distributable Code as part of those applications.
>
> Distribution Requirements. For any Distributable Code you distribute, you must:
> - add significant primary functionality to it in your applications;
> - require distributors and external end users to agree to terms that protect
>   the Distributable Code at least as much as these license terms; and
> - indemnify, defend, and hold harmless Microsoft from any claims, including
>   attorneys' fees, related to the distribution or use of your applications,
>   except to the extent that any claim is based solely on the Distributable
>   Code.
>
> Distribution Restrictions. You may not:
> - distribute Unreleased Components;
> - distribute software developed with Insiders Channel Software or that
>   depends on Unreleased Components;
> - use Microsoft's trademarks in your applications' names or in a way that
>   suggests your applications come from or are endorsed by Microsoft; or
> - modify or distribute the source code of any Distributable Code so that any
>   part of it becomes subject to an Excluded License. An Excluded License is
>   one that requires, as a condition of use, modification or distribution of
>   code, that (i) it be disclosed or distributed in source code form; or (ii)
>   others have the right to modify it.

How the 2022 terms differ:

- Visual Studio Enterprise/Professional 2022 has the same three requirements,
  the indemnity included, and points to `https://aka.ms/vs/17/redistribution`.
  Its first restriction reads "distribute components or runtimes that are
  marked "Preview", "Pre-release", or "Beta";".
- Visual Studio Community 2022 has only two requirements: "add significant
  primary functionality to it in your applications; and require distributors
  and external end users to agree to terms that protect the Distributable Code
  at least as much as this agreement." It has no indemnity item. Its
  restrictions are those of Enterprise 2022.

### Distributable List

`https://aka.ms/vs/18/redistribution` leads to Microsoft's
[Visual Studio Redistribution](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
page, and `https://aka.ms/vs/17/redistribution` to
[Visual Studio 2022 Redistribution](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution).
The 2026 page, verbatim:

> The following section is the "Distributable List" or the "REDIST.TXT" that
> is referenced in the "Distributable Code" section of the Microsoft Software
> License Terms for Visual Studio Enterprise, Visual Studio Professional,
> Visual Studio Community, and Visual Studio Build Tools ("the software"). If
> you have a validly licensed copy of such software, you may copy and
> distribute with your program the unmodified form of the files listed below,
> subject to the License Terms for the software.
>
> Important — Files and components marked as "pre-release" or "preview" in
> the software are not licensed for redistribution.

and, under "Visual C++ Runtime Files":

> Subject to the License Terms for the software, you may copy and distribute
> with your program any of the files within the following folder and its
> subfolders except as noted below. You may not modify these files.
> - [VisualStudioFolder]\VC\redist

The files it then excludes are those of the `debug_nonredist` folders under
`VC\Redist\MSVC\[version]`. The 2022 page has the same grant and exclusions.
The DLLs the ZIP carries come from `Microsoft.VC<toolset>.CRT` inside
`VC\Redist` and outside `debug_nonredist`. That grant reads "copy and
distribute with your program", which covers a ZIP with the DLLs next to the
program.

## How the release meets each condition

This is the project's reading of each condition.

- **Distributable List.** The files come unmodified from the toolset's
  `VC\Redist` runtime folder, never from `debug_nonredist` or System32, and
  the release scripts refuse anything else (above).
- **Significant primary functionality.** dsd-neo is a complete decoder
  program; the runtime only supports it.
- **Terms at least as protective.** The ZIP carries
  `licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`. It says the files
  are Microsoft's, not dsd-neo's, and not licensed under the GPL, and sets
  these terms for them alone: no modification; no reverse engineering,
  decompiling or disassembling; distribution only unmodified, as part of
  dsd-neo and under the same terms. It also says these terms do not limit any
  GPL right in dsd-neo or the rest of the package, since GPLv3 section 10
  forbids further restrictions on the covered work, and the Microsoft files
  are not that work. The terms are the project's choice of terms that protect
  the files at least as much as Microsoft's; Microsoft's license text is not
  reproduced. Microsoft publishes no guidance on how end users of a ZIP with
  no installer "agree to terms". **Not verified.**
- **Indemnity.** Under the Visual Studio 2026 terms, Community included, the
  distributor must indemnify Microsoft as quoted above. The release job uses
  Visual Studio 2026 (below), so this is an obligation on the maintainer, who
  publishes the releases. It is accepted. It is not a GPL conflict. Anyone
  else who builds and distributes the ZIP takes on the obligations of their
  own Visual Studio licence.
- **Trademarks.** The program's name, dsd-neo, uses no Microsoft trademark.
  The notice names Microsoft, Visual Studio and Visual C++ only to identify
  the files, and says dsd-neo is not made, endorsed or supported by Microsoft.
- **Excluded License.** The restriction is about modifying or distributing the
  *source code* of Distributable Code so that it falls under such a license.
  The project ships the unmodified object code and neither modifies nor
  distributes Microsoft's source. GPLv3 does not claim these files: as System
  Libraries they are outside the Corresponding Source (below).
- **Unreleased Components.** The release job uses the generally available
  toolset of the runner's Visual Studio, not a preview toolset. That a given
  run's toolset is not marked preview is **not verified** run by run.

## Build basis

Releases are built on GitHub's hosted Windows runners (`windows-latest`). The
licence the release relies on is the maintainer's own Visual Studio Community
licence. Visual Studio Community 2026 grants:

> Individual License. If you are an individual working on your own
> applications, either to sell or for any other purpose, you may use the
> software to develop and test those applications.

and, for organizations:

> Any number of your users may use the software to develop and test:
> applications released under Open Source Initiative (OSI) approved open
> source software licenses

dsd-neo is released under GPL-3.0-or-later, an OSI-approved license. The same
terms cover machines hosted on Microsoft Azure ("Build Devices"):

> Build Devices and Visual Studio Build Tools. You may copy and install files
> from the software or from Visual Studio Build Tools onto your build devices,
> including physical devices and virtual machines or containers on those
> devices, whether on-premises or remote machines that are (i) owned by you,
> (ii) hosted on Microsoft Azure for you, or (iii) under the day-to-day
> management and control of an Authorized Outsourcer (as defined below) and
> dedicated solely to your use (collectively, "Build Devices"). You and others
> in your organization may use these files on your Build Devices solely to
> compile, build, and verify applications developed by using the software, or
> run quality or performance tests of those applications as part of the build
> process.

GitHub's [GitHub-hosted runners](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)
page says: "GitHub hosts Windows runners … on virtual machines in Azure."

Limits of this basis:

- The runner image comes with Visual Studio **Enterprise** installed
  ([actions/runner-images](https://github.com/actions/runner-images);
  `windows-latest` is the Windows Server 2025 image with Visual Studio
  Enterprise 2026). No public GitHub or Microsoft statement covering the
  licensing of that preinstalled copy was found. **Not verified.**
- The files the release uses come from that Enterprise installation. That its
  `VC\Redist` and toolset files are the same as a Community installation's is
  assumed. **Not verified.**
- The Visual Studio Build Tools licence alone does not cover building one's
  own program: "Use of the software requires a valid license to one of the
  Visual Studio Products". Anyone else who builds and publishes the ZIP, a
  fork for example, needs their own Visual Studio licence.

## GPL: a known, accepted risk

The GPLv3 text and the FSF's published reading of it disagree about shipping
these DLLs with a GPL program. The project keeps shipping them and records the
disagreement as a known, accepted risk.

### What supports shipping them

GPLv3 section 1 ([LICENSE](../LICENSE)) defines System Libraries:

> The "System Libraries" of an executable work include anything, other than
> the work as a whole, that (a) is included in the normal form of packaging a
> Major Component, but which is not part of that Major Component, and (b)
> serves only to enable use of the work with that Major Component, or to
> implement a Standard Interface for which an implementation is available to
> the public in source code form. A "Major Component", in this context, means
> a major essential component (kernel, window system, and so on) of the
> specific operating system (if any) on which the executable work runs, or a
> compiler used to produce the work, or an object code interpreter used to
> run it.

and leaves them out of the Corresponding Source:

> However, it does not include the work's System Libraries, or general-purpose
> tools or generally available free programs which are used unmodified in
> performing those activities but which are not part of the work.

Section 6:

> A separable portion of the object code, whose source code is excluded from
> the Corresponding Source as a System Library, need not be included in
> conveying the object code work.

The project's reading: the runtime DLLs are included in the normal form of
packaging the Visual C++ compiler, which built the work, and they serve only
to enable use of the work with it (`vcruntime140*`) or implement the C++
Standard Library, a standard interface with implementations public in source
form (`msvcp140*`). Nothing in this definition depends on whether the library
ships with the program, and section 6 says such a portion "need not be
included", which assumes it may be.

GPLv2 section 3 excluded such a component "unless that component itself
accompanies the executable". GPLv3 removed those words on purpose. The FSF's
[GPLv3 first-draft rationale](https://gplv3.fsf.org/gpl-rationale-2006-01-16.html)
(2006-01-16):

> As revised, the exception has two parts. Part (a) rewords the GPLv2
> exception for clarity but also removes the words ``unless that component
> itself accompanies the executable.'' By itself, (a) would be too permissive,
> allowing distributors to evade their responsibilities under the GPL. We have
> therefore added part (b) to specify when a system library that is an adjunct
> of a major essential operating system component, compiler, or interpreter
> does not trigger the requirement to distribute source code.

### What goes against it

The FSF's GPL FAQ, entry
[#WindowsRuntimeAndGPL](https://www.gnu.org/licenses/gpl-faq.html#WindowsRuntimeAndGPL):

> You may link your program to these libraries, and distribute the compiled
> program to others. When you do this, the runtime libraries are "System
> Libraries" as GPLv3 defines them. That means that you don't need to worry
> about including their source code with the program's Corresponding Source.
> GPLv2 provides a similar exception in section 3.
>
> You may not distribute these libraries in compiled DLL form with the
> program. To prevent unscrupulous distributors from trying to use the System
> Library exception as a loophole, the GPL says that libraries can only
> qualify as System Libraries as long as they're not distributed with the
> program itself. If you distribute the DLLs with the program, they won't be
> eligible for this exception anymore; then the only way to comply with the
> GPL would be to provide their source code, which you are unable to do.

The FAQ is not the license, but it is the license steward's published
reading. Its statement that "the GPL says" this matches GPLv2's wording, not
GPLv3's.

### Why it stays a risk

- dsd-neo contains GPL code from other copyright holders, who could rely on
  the FAQ. The maintainer cannot grant a GPLv3 section 7 exception for their
  code, so an exception alone cannot remove the risk.
- Other GPL programs built with Visual C++ ship the same DLLs next to their
  program.
- If this risk becomes unacceptable, the fallback is to stop shipping the DLLs
  and ask users to install Microsoft's
  [Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

## Sources

All read 2026-11-18.

- The Visual Studio license terms and REDIST lists above.
- Microsoft's [Redistribute Visual C++ Files](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170),
  [Determine which DLLs to redistribute](https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute?view=msvc-170)
  and [Deployment in Microsoft C++](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp?view=msvc-170).
- GitHub's [GitHub-hosted runners](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)
  and the [runner image list](https://github.com/actions/runner-images).
- The GPLv3 text in [LICENSE](../LICENSE), the same as
  <https://www.gnu.org/licenses/gpl-3.0.txt>. www.gnu.org refused connections
  on that date, so it and the
  [GPL FAQ](https://www.gnu.org/licenses/gpl-faq.html) were read from Internet
  Archive snapshots taken 2026-11-17.
- The GPLv2 text, <https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>, read
  from the SPDX license list's copy.
- The FSF's [GPLv3 first-draft rationale](https://gplv3.fsf.org/gpl-rationale-2006-01-16.html).

Microsoft says its license terms may change; the current versions are at
<https://aka.ms/VisualStudioLicenses>. Check the Distributable Code section
and the REDIST list again before each release.
