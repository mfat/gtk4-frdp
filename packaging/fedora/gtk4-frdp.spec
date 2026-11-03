Name:           gtk4-frdp
Version:        0.1.0
Release:        1%{?dist}
Summary:        GTK 4 widget for embedding FreeRDP 3 remote desktops

License:        GPL-3.0-or-later AND LGPL-3.0-or-later AND Apache-2.0
URL:            https://github.com/mfat/gtk4-frdp
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson >= 0.62
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(freerdp3) >= 3.11
BuildRequires:  pkgconfig(freerdp-client3) >= 3.11
BuildRequires:  pkgconfig(winpr3) >= 3.11
BuildRequires:  gobject-introspection-devel

%description
gtk4-frdp provides FrdpDisplay, a GTK 4 widget that shows a remote desktop
over RDP using FreeRDP 3: keyboard and mouse input, dynamic resolution,
clipboard, audio and certificate/login prompts. Each connection runs on its
own thread, so connecting never blocks the user interface. GObject
Introspection data (GtkFrdp-4.0) is included for Python and other languages.

%package        devel
Summary:        Development files for %{name}
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(gtk4)

%description    devel
Header files, pkg-config file and GIR XML for developing with %{name}.

%prep
%autosetup

%build
# Tests need a display and (for the live suite) an RDP server.
%meson -Dtests=false -Dexamples=false -Ddocumentation=false
%meson_build

%install
%meson_install

%files
%license COPYING LICENSES/*
%doc README.md NEWS.md NOTICE
%{_libdir}/libgtk4-frdp-4.0.so.0*
%{_libdir}/girepository-1.0/GtkFrdp-4.0.typelib

%files devel
%{_includedir}/gtk4-frdp/
%{_libdir}/libgtk4-frdp-4.0.so
%{_libdir}/pkgconfig/gtk4-frdp-4.0.pc
%{_datadir}/gir-1.0/GtkFrdp-4.0.gir

%changelog
* Tue Nov 03 2026 Mehdi <mah.fat@gmail.com> - 0.1.0-1
- Initial package
