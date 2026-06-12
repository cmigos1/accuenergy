<#
  verify-accuenergy-80.ps1
  Rode ISTO DENTRO da sessao RDP do .80 (desktop do servidor).
  - Verifica se broker (mosquitto) + logger (python) estao rodando
  - Mostra se os CSVs ainda estao crescendo (logging ativo)
  - Empacota medicoes.csv + harmonicas.csv num ZIP no Desktop p/ voce puxar via RDP
#>

$base = 'C:\Users\Public\accuenergy'

Write-Host "==================== PROGRAMA RODANDO? ====================" -ForegroundColor Cyan
$procs = Get-Process mosquitto, python -ErrorAction SilentlyContinue
if ($procs) {
    $procs | Select-Object Name, Id, @{n = 'RAM(MB)'; e = { [int]($_.WS / 1MB) } } | Format-Table -AutoSize
} else {
    Write-Host "  NENHUM processo mosquitto/python encontrado!" -ForegroundColor Red
}

Write-Host "==================== CONEXOES NA 1883 ====================" -ForegroundColor Cyan
Get-NetTCPConnection -LocalPort 1883 -ErrorAction SilentlyContinue |
    Where-Object State -in 'Listen', 'Established' |
    Select-Object State, LocalAddress, RemoteAddress | Format-Table -AutoSize
Write-Host "  (RemoteAddress 192.168.1.99 = ESP conectado)" -ForegroundColor DarkGray

Write-Host "==================== CSVs: ESTADO ====================" -ForegroundColor Cyan
function Lines($p) { if (Test-Path $p) { (Get-Content $p | Measure-Object -Line).Lines } else { -1 } }
foreach ($f in 'medicoes.csv', 'harmonicas.csv') {
    $p = Join-Path $base $f
    if (Test-Path $p) {
        $n = Lines $p
        $last = (Get-Content $p -Tail 1)
        Write-Host ("  {0,-16} {1,6} linhas | ultima: {2}" -f $f, $n, $last)
    } else {
        Write-Host "  $f : AUSENTE" -ForegroundColor Red
    }
}

Write-Host "==================== LOGGING ATIVO? (5s) ====================" -ForegroundColor Cyan
$med = Join-Path $base 'medicoes.csv'
$n1 = Lines $med
Start-Sleep -Seconds 5
$n2 = Lines $med
$delta = $n2 - $n1
if ($delta -gt 0) {
    Write-Host "  medicoes.csv: $n1 -> $n2  (+$delta em 5s)  => LOGGING ATIVO" -ForegroundColor Green
} else {
    Write-Host "  medicoes.csv: $n1 -> $n2  (sem crescimento)  => SEM DADOS NOVOS" -ForegroundColor Yellow
}

Write-Host "==================== EMPACOTANDO ====================" -ForegroundColor Cyan
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$zip = "$env:USERPROFILE\Desktop\accuenergy_export_$stamp.zip"
$items = @("$base\medicoes.csv", "$base\harmonicas.csv") | Where-Object { Test-Path $_ }
if ($items) {
    Compress-Archive -Path $items -DestinationPath $zip -Force
    Write-Host "  ZIP criado: $zip" -ForegroundColor Green
    Write-Host "  -> Arraste/copie esse ZIP via RDP (clipboard) para o seu PC." -ForegroundColor Green
    # Se voce habilitou redirecionamento de drive no mstsc, tenta copiar direto:
    $tsdrives = Get-ChildItem '\\tsclient' -ErrorAction SilentlyContinue
    if ($tsdrives) {
        $dest = Join-Path $tsdrives[0].FullName 'accuenergy_export'
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        Copy-Item $zip $dest -Force
        Write-Host "  Copiado tambem para drive redirecionado: $dest" -ForegroundColor Green
    } else {
        Write-Host "  (Nenhum drive redirecionado em \\tsclient — use o clipboard do RDP)" -ForegroundColor DarkGray
    }
} else {
    Write-Host "  Nenhum CSV para empacotar." -ForegroundColor Red
}
