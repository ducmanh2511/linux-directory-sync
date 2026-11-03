# Đồng bộ thư mục giữa hai máy Linux

Bản giữa kỳ môn Hệ điều hành. Chương trình viết bằng C, dùng TCP socket để đồng bộ thư mục từ client lên server theo lệnh. Mỗi kết nối được server giao cho một luồng riêng.

## Mã nguồn

| Tệp | Vai trò |
|---|---|
| `client.c` | Đọc lệnh, duyệt thư mục nguồn, tính SHA-256 và gửi tệp cần cập nhật. |
| `server.c` | Nhận kết nối, tạo luồng, kiểm tra tệp đích và lưu dữ liệu. |
| `Makefile` | Build hai chương trình `server` và `client`. |

## Build và chạy

Trên Ubuntu hoặc WSL Ubuntu, cài các gói sau nếu còn thiếu.

```bash
sudo apt install build-essential libssl-dev
```

Mở terminal trong thư mục chứa `Makefile`. Ví dụ, nếu dự án nằm ở `~/OS_Project`, chạy `cd ~/OS_Project` trước.

**Terminal 1** build rồi bật server. Server dùng cổng TCP 9000 và tạo thư mục `storage2/` tại nơi bạn chạy chương trình.

```bash
make
./server
```

**Terminal 2** đứng cùng thư mục dự án rồi chạy `./client`. Mặc định client kết nối đến `127.0.0.1:9000`. Tại dấu nhắc `sync>`, dùng các lệnh sau.

| Lệnh | Tác dụng |
|---|---|
| `ls` | Liệt kê các mục ngay dưới thư mục lưu trữ của server. |
| `rsync <nguồn> <đích>` | Đẩy tệp mới hoặc đã sửa và bỏ qua tệp có nội dung giống. |
| `quit` | Đóng phiên client. |

`rsync` ở đây là lệnh bên trong chương trình. Nguồn được tính từ thư mục nơi bạn chạy client. Đích được tính từ `storage2/` trên server. Khi nhập lệnh, dùng đường dẫn không có dấu cách. Tên đích dùng chữ ASCII, số, `_`, `-`, `.` hoặc `/`, ví dụ `nhom/baitap`.

## Demo đồng bộ

Nếu thư mục nguồn của bạn là `~/mau` và client chạy từ `~/OS_Project`, nhập từng dòng tại `sync>`.

```text
rsync ../mau baitap
ls
rsync ../mau baitap
```

Nếu dùng thư mục mẫu `mau/` nằm ngay trong dự án, thay `../mau` bằng `mau`.

- Lần đầu với đích mới, client in `PUT OK` cho mỗi tệp gửi thành công.
- Lần chạy lại, tệp có nội dung giống hiện `SAME`.
- Thử sửa một tệp hoặc thêm tệp vào nguồn rồi chạy lại cùng lệnh. Tệp mới hoặc đã sửa hiện `PUT OK`, tệp giữ nguyên hiện `SAME`.

Nếu nguồn có `docs/hello.txt`, kiểm tra bản nhận từ một terminal Bash đứng trong dự án.

```bash
cmp ~/mau/docs/hello.txt storage2/baitap/docs/hello.txt && echo "Hai tep giong nhau"
```

Đổi tên đích khi muốn lặp lại ca gửi lần đầu.

## Demo xử lý đồng thời

**Hai phiên cùng mở.** Giữ client A ở `sync>`. Mở terminal khác trong dự án, chạy `./client` rồi nhập `rsync ../mau baitap2` trong client B. B có thể nhận `PUT OK` khi A vẫn kết nối. Xem hai mã phiên khác nhau trong log `CONNECTED` của server. Ca này kiểm tra server phục vụ nhiều phiên đang mở.

**Cùng cập nhật một tệp đích.** Chuẩn bị hai nguồn trong Bash.

```bash
mkdir -p demo_a/docs demo_b/docs
printf 'BAN A\n' > demo_a/docs/hello.txt
printf 'BAN B\n' > demo_b/docs/hello.txt
```

Trong client A, nhập `rsync demo_a cung_dich`. Trong client B, nhập `rsync demo_b cung_dich`. Nếu cả hai lần cập nhật thành công, đối chiếu thứ tự các dòng `COMMIT cung_dich/docs/hello.txt` trong log server với nội dung tệp cuối.

```bash
cat storage2/cung_dich/docs/hello.txt
```

Server nhận mỗi bản vào tệp tạm riêng. Sau khi nhận đủ và kiểm tra SHA-256, luồng lấy mutex để kiểm tra và thay tệp đích bằng `renameat()`. Bản được thay sau cùng là bản cuối. Quy tắc này áp dụng theo từng tệp. Kết quả cuối chỉ cho biết bản nào được giữ. Để xác nhận hai lượt truyền chồng thời gian, cần quan sát một lượt hoàn thành trong khi lượt kia đang truyền dở.

## Chạy giữa hai máy Linux

Build trên cả hai máy. Máy A chạy `./server` và dùng `hostname -I` để xem địa chỉ IPv4 LAN. Máy B chạy `./client 192.168.1.10`, thay địa chỉ ví dụ bằng IPv4 LAN của A. TCP cổng 9000 cần truy cập được từ máy B. Nhập lệnh đồng bộ ở B và kiểm tra `storage2/` ở A.

## Phạm vi giữa kỳ

Bản này đồng bộ theo lệnh, một chiều từ client lên server, trong phạm vi thêm và cập nhật tệp. Tệp bị xóa ở nguồn vẫn còn ở đích. Lệnh tải về `clone` thuộc phần làm tiếp.
