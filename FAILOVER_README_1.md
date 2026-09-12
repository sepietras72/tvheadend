# Szybszy failover między klientami CA — patch na `descrambler.c` (core)

## Uwaga na start — to jest inne niż CAPMT2/CCcam2

CAPMT2 i CCcam2 to były osobne, dodatkowe pliki — niczego istniejącego nie ruszały.
**Ten patch jest inny: zmienia współdzielony kod `descrambler.c`, z którego korzystają
WSZYSCY klienci CA** (capmt, cwc, cccam, capmt2, cccam2). To większe ryzyko niż
poprzednie zmiany — błąd tutaj potencjalnie wpływa na każdy kanał, nie tylko na nowy,
opcjonalny moduł. Dlatego zanim to wgrasz: przetestuj najpierw na czymś, co nie jest
oglądane na żywo przez domowników, i miej `git stash`/kopię zapasową gotową do szybkiego
cofnięcia.

## Co dokładnie zmienia

Przypomnienie mechanizmu z naszej rozmowy: TVHeadend pyta na starcie kanału wszystkich
włączonych klientów CA naraz, pierwszy który dostarczy klucz — wygrywa (`DS_RESOLVED`)
i ma wyłączność, dopóki jego klucz nie zacznie się spóźniać. Gdy to się stanie,
wywoływany jest `ecm_reset()`, który dotychczas **zawsze** kazał WSZYSTKIM klientom
zacząć od nowa (pełna nowa wymiana ECM) — nawet jeśli inny, nie-aktywny klient miał
w tym samym momencie już gotowy, ważny klucz, tylko został on wcześniej odrzucony
(zignorowany), bo pierwszy klient akurat trzymał wyłączność.

Zmiana:

1. **`descrambler_keys()`** — zamiast całkowicie wyrzucać klucz od "przegranego"
   klienta, zapisuje go teraz jako klucz **zapasowy** bezpośrednio w strukturze tego
   klienta (`td_standby_even/odd/pid/type/valid/time` — nowe pola w `th_descrambler_t`
   w `descrambler.h`).

2. **`ecm_reset()`** — zanim każe wszystkim klientom zaczynać od zera, sprawdza
   najpierw, czy jakiś nie-aktywny klient nie ma już świeżego (młodszego niż 5s)
   klucza zapasowego. Jeśli tak — przełącza się na niego **natychmiast**, przez
   dokładnie tę samą ścieżkę kodu, która normalnie obsługuje pierwsze rozwiązanie
   klucza (`descrambler_keys()`), więc nie jest to nowy, nieprzetestowany mechanizm
   akceptacji klucza — tylko nowy sposób jego *wyzwolenia*.

Efekt: przy zerwaniu głównego źródła (np. dvbapi się zatnie przy przełączeniu CW),
jeśli drugi, niezależny klient (np. CCcam do innego serwera) akurat miał już gotowy
klucz w tym samym momencie — przełączenie następuje bez czekania na kolejną,
pełną wymianę ECM od zera. To realnie powinno skracać czas zamrożenia w scenariuszu,
który testujesz (dvbapi + CCcam jednocześnie).

## Warunek, żeby to w ogóle zadziałało

Musisz mieć **realnie dwóch niezależnych, jednocześnie włączonych klientów CA** dla
tego samego kanału (np. Twój obecny test: dvbapi do swojego + CCcam do innego
serwera). Jeśli masz tylko jednego aktywnego klienta, ten patch nic nie zmienia —
nie ma kogo "awansować" na zapasowego.

## Jak zastosować

```bash
scp failover_patch.patch root@192.168.168.102:/root/test_compile/tvheadend/
cd /root/test_compile/tvheadend
git apply --check failover_patch.patch
git apply failover_patch.patch
make
```

(Patch nie zmienia `configure`/`Makefile` — nie trzeba przebudowywać configure,
te dwa pliki są zawsze kompilowane.)

## Jak sprawdzić, że działa

Włącz debug/trace dla `descrambler` i szukaj w logu linii:

```
fast failover - using cached standby key for service "..." instead of full ECM reset
```

Jeśli ta linia się pojawia w momencie, w którym wcześniej widziałeś zamrożenie —
to znak, że mechanizm faktycznie zadziałał. Jeśli tej linii nie ma, a nadal widać
`key state changed from RESOLVED to READY` z pełnym resetem — oznacza to, że drugi
klient akurat nie miał gotowego klucza w tym oknie 5s (np. bo jego ECM też był
w drodze) — wtedy sam mechanizm nic nie psuje, po prostu nie miał czego użyć.

## Aktualizacja: próg 5s jest teraz konfigurowalny per-CAID

Zamiast zaszytych na sztywno 5 sekund, próg wieku klucza zapasowego jest teraz
nowym polem `standby_age` w tym samym pliku `descrambler`, w którym już masz
`interval`/`paritycheck` — dokładnie ten sam config, ten sam restart, żadnego
nowego mechanizmu do ogarnięcia:

```json
{
  "caid": [
    {
      "name": "moje Nagra 18xx",
      "caid": "1800",
      "mask": "FF00",
      "interval": 10000,
      "paritycheck": 50,
      "standby_age": 5000
    }
  ]
}
```

Wartość w milisekundach. Domyślna (gdy pola brak) to nadal 5000. Jak zawsze —
potwierdzisz, że się wczytało, po linii w logu startowym, teraz z dodatkowym `sa`:

```
adding CAID 1800/FF00 as interval 10000ms pc 50 ep default sa 5000ms (moje Nagra 18xx)
```

Sensowny zakres do testowania: mniejsza wartość (np. 2000-3000ms) = bardziej
rygorystyczne wymaganie świeżości klucza zapasowego, mniejsze ryzyko użycia
czegoś nieaktualnego, ale mniejsza szansa, że akurat coś "złapiesz" na czas.
Większa (np. 8000-10000ms) = więcej szans na skorzystanie z zapasowego klucza,
kosztem odrobinę większego ryzyka, że jest już nieco nieaktualny w momencie użycia.

## Zastrzeżenia

- **Nie kompilowałem ani nie testowałem tego kodu** — jak zawsze, nie mam tu dostępu
  do kompilatora Twojego serwera.
- To dotyka kodu współdzielonego przez wszystkie typy klientów CA — testuj ostrożnie.
