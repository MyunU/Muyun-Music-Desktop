// LX 引擎冒烟测试脚本
// 验证：lx.on / lx.send('inited') / handler 注册 / crypto 绑定 / Promise 返回
lx.on(lx.EVENT_NAMES.request, async function (data) {
  // 证明 lx.utils.crypto 桥接可用
  var sum = lx.utils.crypto.md5('muyun');
  // 证明入参（与真实 LX 一致：{ source, action, info }）可正确读取
  var mid = data.info.musicInfo.songmid;
  return 'https://smoke.example.com/play?source=' + data.source +
         '&mid=' + mid + '&sum=' + sum;
});

lx.send('inited', { status: true, name: 'SmokeTest' });
