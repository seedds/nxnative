#!/usr/bin/env bash
# ==============================================================================
# NxNative Launcher - Script de Sincronização Segura com Upstream (seedds/nxnative)
# ==============================================================================
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔄 [NxNative Launcher] Verificando sincronização com upstream (seedds/nxnative)..."

# 1. Garantir que o remoto upstream está configurado
if ! git remote get-url upstream >/dev/null 2>&1; then
    echo "⚙️ Adicionando remoto upstream (https://github.com/seedds/nxnative.git)..."
    git remote add upstream https://github.com/seedds/nxnative.git
fi

# 2. Buscar atualizações do upstream
echo "📥 Buscando commits do upstream/main..."
git fetch upstream main

# 3. Verificar estado do repositório local
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "⚠️ Atenção: Você está na branch '$CURRENT_BRANCH'. Mudando para 'main'..."
    git checkout main
fi

# 4. Verificar se há alterações não commitadas
if ! git diff-index --quiet HEAD --; then
    echo "⚠️ Existem arquivos modificados no seu diretório de trabalho:"
    git status --short
    echo ""
    echo "💡 Dica: Faça commit ou execute 'git stash' antes de rodar a sincronização para evitar conflitos locais."
    read -p "Deseja salvar alterações temporariamente com git stash e prosseguir? (s/N): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Ss]$ ]]; then
        git stash push -m "Auto-stash antes de sync-upstream em $(date)"
        STASHED=1
    else
        echo "❌ Sincronização cancelada para proteger suas alterações pendentes."
        exit 1
    fi
fi

# 5. Comparar commits
BEHIND_COUNT=$(git rev-list --count HEAD..upstream/main)
if [ "$BEHIND_COUNT" -eq 0 ]; then
    echo "✅ O seu fork já está 100% atualizado com seedds/nxnative:main! Nenhuma ação necessária."
    if [ "${STASHED:-0}" -eq 1 ]; then
        echo "Restaurando alterações locais do stash..."
        git stash pop
    fi
    exit 0
fi

echo "🚀 Encontrados $BEHIND_COUNT novos commits no upstream."
echo "--- Últimos 5 commits no upstream ---"
git log HEAD..upstream/main --oneline -n 5
echo "------------------------------------"

# 6. Tentar mesclagem segura
echo "🔀 Mesclando upstream/main na sua branch main..."
if git merge upstream/main --no-edit -m "chore(sync): merge upstream updates from seedds/nxnative"; then
    echo ""
    echo "🎉 Mesclagem concluída com sucesso e sem nenhum conflito!"
    echo "Suas alterações do NxNative Launcher foram preservadas."
    echo ""
    echo "👉 Para publicar as novidades no seu fork no GitHub, execute:"
    echo "   git push origin main"
else
    echo ""
    echo "⚠️ CONFLITO DE MESCLAGEM DETECTADO!"
    echo "Abordando mesclagem para proteger seu código..."
    git merge --abort
    echo "Sua branch main foi restaurada ao estado original intacto."
    echo "Para inspecionar e resolver os conflitos manualmente, use:"
    echo "   git merge upstream/main"
fi

if [ "${STASHED:-0}" -eq 1 ]; then
    echo ""
    echo "Restaurando suas alterações locais do stash..."
    git stash pop
fi
