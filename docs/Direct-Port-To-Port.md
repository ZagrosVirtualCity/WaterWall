<div dir="rtl">

این روش رو تقریبا همه میشناسن و شاید ساده ترین روش تانل هست 

به این صورت که کاربر به سرور ایران وصل میشه و سرو ایران هم مستقیم به سرور خارج وصل میشه اینجوری اتصال برقرار میشه

فرض شده که ایپی سرور خارج 1.1.1.1 هست

# تک پورت به تک پورت ساده

این روش به دوکودمودور هم معروفه ؛ با آیپی تیبل هم میشه انجامش داد

 البته تفاوت های کوچیکی هست چون دکو توی لایه اپلیکیشن این کار رو انجام میده مثل waterwall

اما آیپی تیبل در لایه های پایین تر nat رو انجام میده
که این خیلی مهم نیست

ما اینجا میخوایم پورت ۴۴۳ سرور ایران رو فوروارد کنیم به ۴۴۳ خارج

فایل کانفیگ در سرور ایران اینجوری نوشته میشه
</div>

```json
{
    "name": "simple_port_to_port",
    "nodes": [
        {
            "name": "input",
            "type": "TcpListener",
            "settings": {
                "address": "0.0.0.0",
                "port": 443,
                "nodelay": true
            },
            "next": "output"
        },
        {
            "name": "output",
            "type": "TcpConnector",
            "settings": {
                "nodelay": true,
                "address": "1.1.1.1",
                "port": 443
            }
        }
    ]
}

```

<div dir="rtl">

این فایل میشه کانفیگ سرور ایران و این روش نیاز به اجرا شدن waterwall روی سرور خارج نداره ؛ فقط سرور ایران کافیه

خوب الان میتونیم یکمی با این بازی بکنیم ؛ مثلا مالتی پورتش کرد

# چند پورت به تک پورت ساده

فایل رو تغییر میدیم جوری که کاربر به هر پورتی که در سرور ایران وصل شد ؛ وصل بشه به پورت ۴۴۳ سرور خارج

</div>

```json
{
    "name": "simple_multiport_to_port",
    "nodes": [
        {
            "name": "input",
            "type": "TcpListener",
            "settings": {
                "address": "0.0.0.0",
                "port": [23,65535],
                "nodelay": true
            },
            "next": "output"
        },
        {
            "name": "output",
            "type": "TcpConnector",
            "settings": {
                "nodelay": true,
                "address": "1.1.1.1",
                "port": 443
            }
        }
    ]
}

```

<div dir="rtl">

تغییر خاصی رخ نداد فقط در حد اینکه پورت رو به صورت بازه وارد کردیم


# چند پورت به چند پورت ساده

حالا میتونیم بازم با همین بازی کنیم و بگیم کاربر به هر پورتی سرور ایران که وصل شد ؛ وصل بشه به همون پورت روی سرور خارج

 این میشه همون ایپی تیبل معروف که همه پورت هارو تونل میکرد ولی خوب ایپی تیبل نیست و بیشتر میشه گفت دوکو مالتی پورت هست چون تو لایه ۷ کار میکنه

</div>

```json
{
    "name": "simple_multiport",
    "nodes": [
        {
            "name": "input",
            "type": "TcpListener",
            "settings": {
                "address": "0.0.0.0",
                "port": [23,65535],
                "nodelay": true
            },
            "next": "output"
        },
        {
            "name": "output",
            "type": "TcpConnector",
            "settings": {
                "nodelay": true,
                "address": "1.1.1.1",
                "port": "src_context->port"
            }
        }
    ]
}
```
<div dir="rtl">


# تک پورت به تک پورت مجزا

همون روشی که اول گفتم که اومدیم پورت 443 ایران رو وصل کردیم به خارج

اگه بخوایم پورت های دیگه هم به صورت تک به تک وصل کنیم مثلا علاوه بر 443 پورت 2083 هم فوروارد کنیم
میتونیم یا مثلا waterwall رو دوبار اجرا کنیم هربار با یه کانفیگ فایل که خوب این راه نیاز نیست چون 
میتونیم توی همین کانفیگ فایل بگیم پورت 2083 هم فوروارد بشه ؛ به این شکل:

</div>

```json
{
    "name": "simple_port_to_port_x2",
    "nodes": [
        {
            "name": "input1",
            "type": "TcpListener",
            "settings": {
                "address": "0.0.0.0",
                "port": 443,
                "nodelay": true
            },
            "next": "output1"
        },
        {
            "name": "output1",
            "type": "TcpConnector",
            "settings": {
                "nodelay": true,
                "address": "1.1.1.1",
                "port": 443
            }
        },

        {
            "name": "input2",
            "type": "TcpListener",
            "settings": {
                "address": "0.0.0.0",
                "port": 2083,
                "nodelay": true
            },
            "next": "output2"
        },
        {
            "name": "output2",
            "type": "TcpConnector",
            "settings": {
                "nodelay": true,
                "address": "1.1.1.1",
                "port": 2083
            }
        }
    ]
}

```

<div dir="rtl">




اینا بخش کوچکی از قابلیت های Node ها بود ؛ حتی میشه این نود ها همشون روی یک پورت ران بشن و بر اساس ایپی مبدا به مقاصد مختلف فوروارد کنن  
و همچنین کلی node مختلف برای کار های مختلف مثل رمزنگاری ؛ کانکشن از پیش هند شیک شده ؛ کاکشن معکوس ؛ Mux و غیره هستند که اروم اروم در روش های بعد میریم سراغشون


</div>