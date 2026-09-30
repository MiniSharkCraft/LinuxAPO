# Chuyển cấu hình từ Windows Equalizer APO sang SkyAPO

Tài liệu này hướng dẫn chuyển một cấu hình Equalizer APO (EAPO) hiện có sang
SkyAPO trên Linux. SkyAPO dùng các filter DSP upstream EAPO ở những phần đã
được port, nhưng **không phải** bản tương thích đầy đủ của Windows EAPO và
không có bộ chuyển đổi cấu hình tự động. Hãy giữ bản gốc, chuyển từng phần,
kiểm tra bằng CLI, rồi mới chọn cấu hình đó cho daemon.

## 1. Sao lưu và tìm cấu hình Windows

Sao lưu toàn bộ thư mục cấu hình EAPO, không chỉ `config.txt`: các lệnh
`Include:` và filter/plugin có thể tham chiếu tới file khác. Trong Windows,
dùng Configuration Editor hoặc cấu hình cài đặt của máy để xác định file
đang hoạt động; đừng giả định mọi máy đều dùng cùng một đường dẫn. Chép các
file cần thiết sang Linux và kiểm tra lại đường dẫn tương đối, tên file, quyền
đọc và định dạng audio của IR.

SkyAPO lưu cấu hình mặc định ở:

```text
$XDG_CONFIG_HOME/skyapo/config.txt
```

Khi `XDG_CONFIG_HOME` chưa đặt, đường dẫn là `~/.config/skyapo/config.txt`.
SkyAPO không đọc Windows Registry, không cài APO vào audio engine, và không
tự chọn microphone theo cấu hình Windows.

## 2. Chọn capture device Linux

SkyAPO xử lý audio từ một PipeWire capture device được chọn rõ ràng. Liệt kê
và chọn thiết bị bằng node name ổn định:

```sh
skyapo device list
skyapo device set alsa_input.usb-Example_Microphone-00.mono-fallback
skyapo device current
```

Thay ví dụ node name bằng giá trị `NAME` từ lệnh `device list`. Có thể truyền
ID số đang tồn tại trong phiên PipeWire hiện tại, nhưng ID runtime có thể đổi
sau khi khởi động lại; SkyAPO lưu lựa chọn theo node name. Không chọn
`SkyAPO Virtual Mic` làm input của chính SkyAPO, nếu không sẽ tạo vòng lặp.

Sau khi chọn thiết bị, kiểm tra cấu hình và khởi động daemon:

```sh
skyapo config check /path/to/config.txt
skyapo start
skyapo status
```

`skyapo config check --json /path/to/config.txt` trả JSON để tích hợp vào
script/editor. Việc check hiện khởi tạo theo stereo 48 kHz; nó không chứng
minh cấu hình hợp lệ cho mọi layout hoặc sample rate thực tế. Kiểm tra status
để xem format đã thương lượng và chọn `SkyAPO Virtual Mic` làm microphone
trong ứng dụng Linux.

## 3. Các lệnh cấu hình

| Cấu hình EAPO | Tình trạng trong SkyAPO | Việc cần làm khi chuyển |
| --- | --- | --- |
| `Preamp:` | Được hỗ trợ bằng upstream `PreampFilter`. | Thường có thể giữ nguyên; kiểm tra đơn vị dB và kết quả bằng render/test. |
| Parametric `Filter: ON PK Fc … Hz Gain … dB Q …` | Được hỗ trợ bằng upstream BiQuad. | Giữ từng dòng rồi xác thực tần số dưới Nyquist của thiết bị. |
| `Filter: ON IIR …` | Hỗ trợ dạng IIR mà parser hiện tại chấp nhận. | Chuyển thử và chạy `config check`; không giả định mọi biến thể Filter EAPO đều được hỗ trợ. |
| `Delay:` | Được hỗ trợ qua upstream filter. | Xác thực đơn vị/cú pháp bằng parser; lưu ý layout đầu ra vẫn cố định. |
| `Channel:` và `Copy:` | Hỗ trợ chọn/remap channel trong layout hiện có qua upstream `FilterConfiguration`. | So khớp tên channel Linux (ví dụ `L`, `R`, `C`, `LFE`, `RL`, `RR`, `SL`, `SR`). Không thể thêm output channel mới vượt layout mic ảo cố định. |
| `Include:` | Được hỗ trợ; file con được mở rộng tại vị trí directive. | Giữ cấu trúc tương đối nếu chép nguyên cây file. Đường dẫn tương đối tính từ file đang include; đường dẫn có dấu cách hoặc `#` đặt trong dấu nháy kép. Kiểm tra từng file phụ thuộc. |
| `GraphicEQ:` | Dựa trên upstream DSP; chỉ có khi build tìm thấy FFTW3f. | Nếu directive bị báo không khả dụng, cài dependency/build phù hợp hoặc bỏ directive có chủ ý; không coi là filter đã áp dụng. |
| `Convolution:` | Dựa trên upstream convolution; cần FFTW3f và đọc IR qua adapter Linux/libsndfile. | Chép IR sang Linux, sửa path, xác thực sample rate/đọc được. Bộ lọc cần block cố định; offline renderer zero-pad block cuối. |
| `If:`, `ElseIf:`, `Else:`, `EndIf:` | Có subset biểu thức số/boolean khi build với muParser. | Chỉ dùng `sampleRate`, `inputChannelCount`, `outputChannelCount` và toán tử subset đã ghi trong `CONFIG.md`; đây không phải muParserX đầy đủ. |
| `Stage:` | `capture` ánh xạ vào đường capture Linux; các stage Windows `pre-mix` và `post-mix` bị bỏ qua. | Đặt filter cần chạy trên microphone trong `capture`. Đừng dựa vào stage bị bỏ qua để tạo xử lý tương đương. |
| `LoudnessCorrection:` | Dùng filter/factory DSP upstream trong daemon; volume đầu vào là gain per-channel đồng nhất của default PipeWire render sink khi truy xuất được. Offline renderer/config checker từ chối vì không có endpoint volume live. | Cần kiểm tra `skyapo status`: nguồn volume không phải bản sao tuyệt đối của Windows `IAudioEndpointVolume`, và filter được giữ neutral khi snapshot chưa khả dụng. |

Các directive không hỗ trợ trong stage/nhánh đang hoạt động sẽ gây lỗi có
đường dẫn và số dòng; SkyAPO không âm thầm bỏ qua. Nhánh conditional sai và
Windows stage bị bỏ qua không được parse như lệnh đang hoạt động. Parser hiện
chưa tương đương toàn bộ parser EAPO: inline backtick expansion, string/matrix,
registry/regex functions và Windows registry reads không được hỗ trợ. Xem
[`CONFIG.md`](CONFIG.md) để biết chi tiết và định dạng diagnostics.

Ví dụ tối thiểu có thể dùng làm cấu hình SkyAPO:

```text
Preamp: -6 dB
Filter: ON PK Fc 100 Hz Gain 6 dB Q 1.0
```

Ví dụ trên dùng đúng cú pháp parametric hiện có. Không gộp nhiều thay đổi vào
lần chuyển đầu tiên: thêm từng include/filter, rồi chạy `skyapo config check`
lại sau mỗi nhóm.

## 4. Plugin và lệnh Windows không được chuyển tự động

Lệnh plugin EAPO/Windows VST không thể xem như tương thích nguyên trạng. SkyAPO
có cú pháp riêng cho các host tùy chọn; plugin phải được cài và tìm thấy ở
Linux trước:

```text
Plugin: LV2 https://example.org/plugins/my-effect
Plugin: CLAP org.vendor.plugin-id
Plugin: VST3 <class-UID>
```

`LV2`, `CLAP`, `VST3` chỉ dùng được khi build tương ứng có host/dependency. Tham
số khởi tạo cũng khác theo format: LV2 dùng `symbol=value`, CLAP và VST3 dùng
ID (ưu tiên) hoặc tên duy nhất như mô tả trong [`PLUGINS.md`](PLUGINS.md).
Plugin host hiện không cung cấp state serialization, live automation/bypass,
plugin UI, latency compensation hay crash isolation; plugin chạy trong tiến
trình daemon. Chỉ chuyển plugin sau khi thử riêng trên đúng số channel.

VST2 **chưa được triển khai** và prototype hiện có không phải host được hỗ trợ.
Windows VST có thể dùng wrapper VST3 do yabridge tạo nếu tương thích, nhưng
SkyAPO không quản lý yabridge/Wine và không đảm bảo mọi plugin hoạt động.
Không đưa đường dẫn Windows `.dll`, COM/APO registration, registry settings,
Windows device IDs, GUI effects hay script cài đặt Windows vào config Linux.

## 5. Quy trình kiểm tra và sử dụng

1. Giữ bản sao nguyên vẹn của cấu hình và các file include/IR/plugin preset.
2. Chuyển một nhóm directive nhỏ vào file Linux; chỉnh path và `Stage:` nếu
   cần.
3. Chạy `skyapo config check /path/to/config.txt`; xử lý mọi lỗi thay vì xóa
   directive chỉ để vượt qua parser.
4. Với file WAV, dùng renderer để kiểm tra kết quả offline:

   ```sh
   skyapo-render --input input.wav --output output.wav --config /path/to/config.txt
   ```

   Renderer bảo toàn sample rate/channel count đầu vào và xuất WAV float. Đây
   là phép thử DSP offline, không xác nhận routing microphone.
5. Chạy cấu hình bằng daemon, xem `skyapo status`, rồi kiểm tra graph bằng
   `wpctl status` hoặc `pw-dump`. Chọn `SkyAPO Virtual Mic` trong ứng dụng đích.
6. So sánh mức/tín hiệu trước-sau bằng phép đo phù hợp; nghe thử một mình
   không đủ để xác nhận gain, channel map hay plugin.

Daemon nạp cấu hình ở startup và theo dõi thay đổi; reload lỗi giữ nguyên
engine hợp lệ trước đó. Xem [`REALTIME.md`](REALTIME.md) và
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) nếu thiết bị hoặc virtual source
không xuất hiện.

## Phạm vi tương thích

Hướng dẫn này mô tả khả năng parser/adapter/host đang có, không tuyên bố tương
thích toàn bộ cấu hình EAPO. Kết quả `config check` chỉ chứng minh cấu hình
qua bước parse/khởi tạo ở tham số validation hiện tại; xác minh runtime trên
thiết bị thật và đo audio vẫn cần thiết. Danh sách lệnh được hỗ trợ có thể
thay đổi theo build tùy chọn và phiên bản SkyAPO; khi có khác biệt, source,
`CONFIG.md` và thông báo parser đang chạy là nguồn xác thực.
