Name:       ngfd-plugin-pulse
Summary:    PulseAudio sound effect plugin for ngfd
Version:    0.91.0.1
Release:    1
Group:      System/Daemons
License:    LGPL 2.1
URL:        https://github.com/mer-hybris/ngfd-plugin-pulse
Source:     %{name}-%{version}.tar.gz
Requires:   ngfd >= 0.91
BuildRequires:  cmake
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(ngf-plugin) >= 0.91
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(sndfile)

%description
This package contains a low-latency sound effect playback plugin for
the non-graphical feedback daemon using libsndfile and PulseAudio.

%prep
%setup -q -n %{name}-%{version}

%build
%cmake
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%post
if [ "$1" -ge 1 ]; then
    systemctl-user daemon-reload || true
    systemctl-user restart ngfd.service || true
fi

%postun
if [ "$1" -eq 0 ]; then
    systemctl-user stop ngfd.service || true
    systemctl-user daemon-reload || true
fi

%files
%defattr(-,root,root,-)
%doc README COPYING
%{_libdir}/ngf/libngfd_pulse.so
